#include "OpenXRPresenter.h"

#if defined(RG_WITH_OPENXR)

#include "RgException.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <sstream>
#include <ranges>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace RTGL1
{
namespace
{

#ifdef _WIN32
void* LoadOpenXRLoader()
{
    return static_cast<void*>(LoadLibraryA("openxr_loader.dll"));
}
void UnloadOpenXRLoader(void* loader)
{
    if (loader) FreeLibrary(static_cast<HMODULE>(loader));
}
void* GetLoaderSymbol(void* loader, const char* name)
{
    return loader ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(loader), name)) : nullptr;
}
#else
void* LoadOpenXRLoader()
{
    return dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
}
void UnloadOpenXRLoader(void* loader)
{
    if (loader) dlclose(loader);
}
void* GetLoaderSymbol(void* loader, const char* name)
{
    return loader ? dlsym(loader, name) : nullptr;
}
#endif

std::vector<std::string> SplitExtensions(const char* text)
{
    std::istringstream input(text ? text : "");
    std::vector<std::string> result;
    std::string item;
    while (input >> item) result.push_back(item);
    return result;
}

bool HasExtension(const std::vector<XrExtensionProperties>& extensions, const char* name)
{
    return std::ranges::any_of(extensions, [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

}


float VectorLength(const XrVector3f& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

XrVector3f NormalizeVector(const XrVector3f& v)
{
    const float length = VectorLength(v);
    if( length <= 0.00001f ) return { 0, 0, -1 };
    return { v.x / length, v.y / length, v.z / length };
}

XrVector3f RotateVector(const XrQuaternionf& q, const XrVector3f& v)
{
    const XrVector3f qv{ q.x, q.y, q.z };
    const XrVector3f t{
        2.0f * (qv.y * v.z - qv.z * v.y),
        2.0f * (qv.z * v.x - qv.x * v.z),
        2.0f * (qv.x * v.y - qv.y * v.x),
    };
    return {
        v.x + q.w * t.x + qv.y * t.z - qv.z * t.y,
        v.y + q.w * t.y + qv.z * t.x - qv.x * t.z,
        v.z + q.w * t.z + qv.x * t.y - qv.y * t.x,
    };
}

XrQuaternionf NormalizeQuaternion(const XrQuaternionf& q)
{
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if( length <= 0.00001f ) return { 0, 0, 0, 1 };
    return { q.x / length, q.y / length, q.z / length, q.w / length };
}

XrQuaternionf YawQuaternion(float yaw)
{
    return { 0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f) };
}

XrVector3f HeadHorizontalForward(const XrPosef& headPose)
{
    const XrVector3f forward = RotateVector(headPose.orientation, { 0, 0, -1 });
    return NormalizeVector({ forward.x, 0, forward.z });
}

XrPosef BuildYawUprightAnchor(const XrPosef& headPose, float distance, float verticalPosition)
{
    const XrVector3f forward = HeadHorizontalForward(headPose);
    const float yaw = std::atan2(forward.x, -forward.z);
    return {
        YawQuaternion(-yaw),
        {
            headPose.position.x + forward.x * distance,
            headPose.position.y + verticalPosition,
            headPose.position.z + forward.z * distance,
        },
    };
}

float AngleDifference(float a, float b)
{
    constexpr float pi = 3.14159265358979323846f;
    float difference = std::fmod(a - b + pi, pi * 2.0f);
    if( difference < 0 ) difference += pi * 2.0f;
    return std::fabs(difference - pi);
}

XrPosef BlendPose(const XrPosef& from, const XrPosef& to, float amount)
{
    amount = std::clamp(amount, 0.0f, 1.0f);
    XrQuaternionf destination = to.orientation;
    const float dot = from.orientation.x * destination.x + from.orientation.y * destination.y +
                      from.orientation.z * destination.z + from.orientation.w * destination.w;
    if( dot < 0.0f )
    {
        destination = { -destination.x, -destination.y, -destination.z, -destination.w };
    }
    return {
        NormalizeQuaternion({
            from.orientation.x + (destination.x - from.orientation.x) * amount,
            from.orientation.y + (destination.y - from.orientation.y) * amount,
            from.orientation.z + (destination.z - from.orientation.z) * amount,
            from.orientation.w + (destination.w - from.orientation.w) * amount,
        }),
        {
            from.position.x + (to.position.x - from.position.x) * amount,
            from.position.y + (to.position.y - from.position.y) * amount,
            from.position.z + (to.position.z - from.position.z) * amount,
        },
    };
}

float CubicEase(float amount)
{
    amount = std::clamp(amount, 0.0f, 1.0f);
    return amount * amount * (3.0f - 2.0f * amount);
}
OpenXRPresenter::OpenXRPresenter(const RgOpenXRPresentationCreateInfoEXT& info)
    : desktopMirror(info.desktopMirror ? RG_TRUE : RG_FALSE)
    , quadWidthMeters(1.6f)
    , quadDistanceMeters(1.8f)
{
    if (!info.enable) return;

    LoadLoader();

    uint32_t extensionCount = 0;
    if (enumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_RUNTIME_UNAVAILABLE, "OpenXR extension enumeration failed");
    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    if (enumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_RUNTIME_UNAVAILABLE, "OpenXR extension enumeration failed");
    const bool enable2 = HasExtension(extensions, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    const bool enable1 = HasExtension(extensions, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (!enable2 && !enable1)
        Fail(RG_RESULT_OPENXR_VULKAN_REQUIREMENTS_UNSUPPORTED, "OpenXR runtime has no Vulkan graphics extension");

    const char* vulkanExtensions[2]{};
    uint32_t vulkanExtensionCount = 0;
    if( enable1 ) vulkanExtensions[vulkanExtensionCount++] = XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    if( enable2 ) vulkanExtensions[vulkanExtensionCount++] = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.enabledExtensionCount = vulkanExtensionCount;
    createInfo.enabledExtensionNames = vulkanExtensions;
    std::strncpy(createInfo.applicationInfo.applicationName, "DoomXRT", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(createInfo.applicationInfo.engineName, "RTGL1", XR_MAX_ENGINE_NAME_SIZE - 1);
    // XR_API_VERSION_1_0 inherits the SDK's current patch number. Older
    // runtimes can legitimately reject that (for example, 1.0.58), so use
    // the base 1.0 API version that this presenter actually targets.
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    const XrResult instanceResult = createInstance(&createInfo, &xrInstance);
    if (instanceResult != XR_SUCCESS)
    {
        const std::string message = "xrCreateInstance failed: XrResult=" +
                                    std::to_string(static_cast<int64_t>(instanceResult));
        Fail(RG_RESULT_OPENXR_RUNTIME_UNAVAILABLE, message.c_str());
    }

    LoadInstanceFunctions();

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (getSystem(xrInstance, &systemInfo, &systemId) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_RUNTIME_UNAVAILABLE, "OpenXR HMD system selection failed");

    uint32_t length = 0;
    if (getVulkanInstanceExtensions(xrInstance, systemId, 0, &length, nullptr) != XR_SUCCESS || length == 0)
        Fail(RG_RESULT_OPENXR_VULKAN_REQUIREMENTS_UNSUPPORTED, "OpenXR Vulkan instance requirements unavailable");
    std::string instanceNames(length, '\0');
    if (getVulkanInstanceExtensions(xrInstance, systemId, length, &length, instanceNames.data()) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_VULKAN_REQUIREMENTS_UNSUPPORTED, "OpenXR Vulkan instance requirements unavailable");
    instanceExtensionStorage = SplitExtensions(instanceNames.c_str());
    for (const auto& extension : instanceExtensionStorage) instanceExtensions.push_back(extension.c_str());

    length = 0;
    if (getVulkanDeviceExtensions(xrInstance, systemId, 0, &length, nullptr) != XR_SUCCESS || length == 0)
        Fail(RG_RESULT_OPENXR_VULKAN_REQUIREMENTS_UNSUPPORTED, "OpenXR Vulkan device requirements unavailable");
    std::string deviceNames(length, '\0');
    if (getVulkanDeviceExtensions(xrInstance, systemId, length, &length, deviceNames.data()) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_VULKAN_REQUIREMENTS_UNSUPPORTED, "OpenXR Vulkan device requirements unavailable");
    deviceExtensionStorage = SplitExtensions(deviceNames.c_str());
    for (const auto& extension : deviceExtensionStorage) deviceExtensions.push_back(extension.c_str());

    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (getVulkanGraphicsRequirements2(xrInstance, systemId, &requirements) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_VULKAN_REQUIREMENTS_UNSUPPORTED, "OpenXR Vulkan graphics requirements query failed");
}

OpenXRPresenter::~OpenXRPresenter()
{
    DestroySession();
    if (xrInstance != XR_NULL_HANDLE && destroyInstance) destroyInstance(xrInstance);
    UnloadOpenXRLoader(loader);
}

[[noreturn]] void OpenXRPresenter::Fail(RgResult result, const char* reason) const
{
    throw RgException(result, reason);
}

void OpenXRPresenter::LoadLoader()
{
    loader = LoadOpenXRLoader();
    if (!loader) Fail(RG_RESULT_OPENXR_LOADER_UNAVAILABLE, "OpenXR loader could not be loaded");
    getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetLoaderSymbol(loader, "xrGetInstanceProcAddr"));
    enumerateInstanceExtensionProperties = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(GetLoaderSymbol(loader, "xrEnumerateInstanceExtensionProperties"));
    createInstance = reinterpret_cast<PFN_xrCreateInstance>(GetLoaderSymbol(loader, "xrCreateInstance"));
    if (!getInstanceProcAddr || !enumerateInstanceExtensionProperties || !createInstance)
        Fail(RG_RESULT_OPENXR_LOADER_UNAVAILABLE, "OpenXR loader entry points are unavailable");
}

RgResult OpenXRPresenter::SetVirtualScreenSettings(const RgOpenXRVirtualScreenSettingsEXT& settings)
{
    if( settings.sType != RG_STRUCTURE_TYPE_OPENXR_VIRTUAL_SCREEN_SETTINGS_EXT )
        return RG_RESULT_WRONG_STRUCTURE_TYPE;

    const int mode = std::clamp(settings.mode, 0, 3);
    const float size = std::clamp(settings.size, 0.1f, 4.0f);
    const float distance = std::clamp(settings.distance, 0.0f, 20.0f);
    const float verticalPosition = std::clamp(settings.verticalPosition, -10.0f, 10.0f);
    const bool settingsChanged = activeMode != mode ||
        std::fabs(activeDistance - distance) > 0.0001f ||
        std::fabs(activeVerticalPosition - verticalPosition) > 0.0001f;

    virtualScreenSettings = settings;
    virtualScreenSettings.mode = mode;
    virtualScreenSettings.size = size;
    virtualScreenSettings.distance = distance;
    virtualScreenSettings.verticalPosition = verticalPosition;

    if( settings.recenterRequest != consumedRecenterRequest )
    {
        consumedRecenterRequest = settings.recenterRequest;
        anchorInvalidated = true;
    }
    if( settingsChanged ) anchorInvalidated = true;
    activeMode = mode;
    activeDistance = distance;
    activeVerticalPosition = verticalPosition;
    return RG_RESULT_SUCCESS;
}
void OpenXRPresenter::LoadInstanceFunctions()
{
#define LOAD_XR(api, member) do { PFN_xrVoidFunction function = nullptr; if (getInstanceProcAddr(xrInstance, #api, &function) != XR_SUCCESS || !function) Fail(RG_RESULT_OPENXR_RUNTIME_UNAVAILABLE, #api " is unavailable"); member = reinterpret_cast<PFN_##api>(function); } while (false)
#define LOAD_XR_OPTIONAL(api, member) do { PFN_xrVoidFunction function = nullptr; if (getInstanceProcAddr(xrInstance, #api, &function) == XR_SUCCESS) member = reinterpret_cast<PFN_##api>(function); } while (false)
    LOAD_XR(xrDestroyInstance, destroyInstance);
    LOAD_XR(xrGetSystem, getSystem);
    LOAD_XR(xrPollEvent, pollEvent);
    LOAD_XR(xrResultToString, resultToString);
    LOAD_XR(xrCreateSession, createSession);
    LOAD_XR(xrDestroySession, destroySession);
    LOAD_XR(xrBeginSession, beginSession);
    LOAD_XR(xrEndSession, endSession);
    LOAD_XR(xrCreateReferenceSpace, createReferenceSpace);
    LOAD_XR(xrDestroySpace, destroySpace);
    LOAD_XR(xrLocateSpace, locateSpace);
    LOAD_XR(xrEnumerateViewConfigurationViews, enumerateViewConfigurationViews);
    LOAD_XR(xrEnumerateSwapchainFormats, enumerateSwapchainFormats);
    LOAD_XR(xrCreateSwapchain, createSwapchain);
    LOAD_XR(xrDestroySwapchain, destroySwapchain);
    LOAD_XR(xrEnumerateSwapchainImages, enumerateSwapchainImages);
    LOAD_XR(xrWaitFrame, waitFrame);
    LOAD_XR(xrBeginFrame, beginFrame);
    LOAD_XR(xrEndFrame, endFrame);
    LOAD_XR(xrAcquireSwapchainImage, acquireSwapchainImage);
    LOAD_XR(xrWaitSwapchainImage, waitSwapchainImage);
    LOAD_XR(xrReleaseSwapchainImage, releaseSwapchainImage);
    LOAD_XR_OPTIONAL(xrGetVulkanInstanceExtensionsKHR, getVulkanInstanceExtensions);
    LOAD_XR_OPTIONAL(xrGetVulkanDeviceExtensionsKHR, getVulkanDeviceExtensions);
    LOAD_XR_OPTIONAL(xrGetVulkanGraphicsRequirements2KHR, getVulkanGraphicsRequirements2);
    LOAD_XR_OPTIONAL(xrGetVulkanGraphicsDevice2KHR, getVulkanGraphicsDevice2);
    if (!getVulkanInstanceExtensions || !getVulkanDeviceExtensions || !getVulkanGraphicsRequirements2)
        Fail(RG_RESULT_OPENXR_VULKAN_REQUIREMENTS_UNSUPPORTED, "OpenXR Vulkan functions are unavailable");
#undef LOAD_XR_OPTIONAL
#undef LOAD_XR
}

VkPhysicalDevice OpenXRPresenter::GetPreferredPhysicalDevice(VkInstance instance) const
{
    if (!getVulkanGraphicsDevice2) return VK_NULL_HANDLE;
    XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId = systemId;
    info.vulkanInstance = instance;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    if (getVulkanGraphicsDevice2(xrInstance, &info, &device) != XR_SUCCESS || device == VK_NULL_HANDLE)
        Fail(RG_RESULT_OPENXR_VULKAN_REQUIREMENTS_UNSUPPORTED, "OpenXR did not return a Vulkan physical device");
    return device;
}

void OpenXRPresenter::CreateSession(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                                    VkQueue queue, uint32_t queueFamilyIndex)
{
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = instance;
    binding.physicalDevice = physicalDevice;
    binding.device = device;
    binding.queueFamilyIndex = queueFamilyIndex;
    binding.queueIndex = 0;
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId;
    if (createSession(xrInstance, &sessionInfo, &session) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_SESSION_ERROR, "OpenXR Vulkan session creation failed");

    XrReferenceSpaceCreateInfo viewSpaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceInfo.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    if( createReferenceSpace(session, &viewSpaceInfo, &headSpace) != XR_SUCCESS ) headSpace = XR_NULL_HANDLE;

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    spaceInfo.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    if (createReferenceSpace(session, &spaceInfo, &space) != XR_SUCCESS) {
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (createReferenceSpace(session, &spaceInfo, &space) != XR_SUCCESS)
            Fail(RG_RESULT_OPENXR_SESSION_ERROR, "OpenXR reference-space creation failed");
    }

    uint32_t viewCount = 0;
    enumerateViewConfigurationViews(xrInstance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (viewCount) enumerateViewConfigurationViews(xrInstance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data());
    swapchainExtent = {1920, 1080};
    for (const auto& view : views) {
        swapchainExtent.width = std::max(swapchainExtent.width, view.recommendedImageRectWidth);
        swapchainExtent.height = std::max(swapchainExtent.height, view.recommendedImageRectHeight);
    }

    uint32_t formatCount = 0;
    if (enumerateSwapchainFormats(session, 0, &formatCount, nullptr) != XR_SUCCESS || formatCount == 0)
        Fail(RG_RESULT_OPENXR_SWAPCHAIN_ERROR, "OpenXR swapchain format enumeration failed");
    std::vector<int64_t> formats(formatCount);
    enumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
    const VkFormat preferred[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    for (VkFormat candidate : preferred) if (std::ranges::find(formats, static_cast<int64_t>(candidate)) != formats.end()) { swapchainFormat = candidate; break; }
    if (swapchainFormat == VK_FORMAT_UNDEFINED) swapchainFormat = static_cast<VkFormat>(formats.front());

    XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchainInfo.format = swapchainFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = swapchainExtent.width;
    swapchainInfo.height = swapchainExtent.height;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    if (createSwapchain(session, &swapchainInfo, &swapchain) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_SWAPCHAIN_ERROR, "OpenXR quad swapchain creation failed");
    if (enumerateSwapchainImages(swapchain, 0, &swapchainImageCount, nullptr) != XR_SUCCESS || swapchainImageCount == 0)
        Fail(RG_RESULT_OPENXR_SWAPCHAIN_ERROR, "OpenXR swapchain image enumeration failed");
    swapchainImages = new XrSwapchainImageVulkanKHR[swapchainImageCount];
    for (uint32_t i = 0; i < swapchainImageCount; ++i) swapchainImages[i] = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    if (enumerateSwapchainImages(swapchain, swapchainImageCount, &swapchainImageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages)) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_SWAPCHAIN_ERROR, "OpenXR Vulkan swapchain image enumeration failed");

    swapchainAspect = static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height);

    quadLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quadLayer.space = space;
    quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quadLayer.pose = {{0, 0, 0, 1}, {0, 0, -quadDistanceMeters}};
    quadLayer.size = {quadWidthMeters, quadWidthMeters * static_cast<float>(swapchainExtent.height) / static_cast<float>(swapchainExtent.width)};
    quadLayer.subImage.swapchain = swapchain;
    quadLayer.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(swapchainExtent.width), static_cast<int32_t>(swapchainExtent.height)}};
}

void OpenXRPresenter::PollEvents()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (pollEvent(xrInstance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            anchorInvalidated = true;
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            sessionState = changed.state;
            if (sessionState == XR_SESSION_STATE_STOPPING && sessionRunning) { endSession(session); sessionRunning = false; anchorInvalidated = true; }
            if (sessionState == XR_SESSION_STATE_VISIBLE) anchorInvalidated = true;
            if (sessionState == XR_SESSION_STATE_EXITING || sessionState == XR_SESSION_STATE_LOSS_PENDING) { sessionRunning = false; frameActive = false; anchorInvalidated = true; }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool OpenXRPresenter::LocateHeadPose(XrTime displayTime, XrPosef& pose)
{
    if( headSpace == XR_NULL_HANDLE || space == XR_NULL_HANDLE || locateSpace == nullptr ) return false;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    const XrResult result = locateSpace(headSpace, space, displayTime, &location);
    if( result != XR_SUCCESS ) return false;
    const XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if( (location.locationFlags & required) != required ) return false;
    pose = location.pose;
    return true;
}

void OpenXRPresenter::UpdateQuadPose(XrTime displayTime)
{
    XrPosef headPose{};
    if( !LocateHeadPose(displayTime, headPose) ) return;

    const float distance = virtualScreenSettings.distance > 0.001f ? virtualScreenSettings.distance : 1.8f;
    const float width = 1.6f * std::clamp(virtualScreenSettings.size, 0.1f, 4.0f);
    const float aspect = swapchainAspect > 0.001f ? swapchainAspect : 16.0f / 9.0f;
    quadLayer.size = { width, width / aspect };

    const int mode = virtualScreenSettings.mode == 0 ? 2 : std::clamp(virtualScreenSettings.mode, 1, 3);
    if( mode == 3 )
    {
        const XrVector3f offset = RotateVector(headPose.orientation, { 0, 0, -distance });
        quadLayer.pose = {
            NormalizeQuaternion(headPose.orientation),
            { headPose.position.x + offset.x,
              headPose.position.y + offset.y + virtualScreenSettings.verticalPosition,
              headPose.position.z + offset.z }
        };
        return;
    }

    const XrPosef candidateAnchor =
        BuildYawUprightAnchor(headPose, distance, virtualScreenSettings.verticalPosition);
    const XrVector3f candidateForward = RotateVector(candidateAnchor.orientation, { 0, 0, -1 });
    const float candidateYaw = std::atan2(candidateForward.x, -candidateForward.z);

    if( anchorInvalidated || !anchorValid )
    {
        stationaryAnchor = candidateAnchor;
        targetAnchor = candidateAnchor;
        currentAnchor = candidateAnchor;
        anchorTransitionFrom = candidateAnchor;
        anchorTransitionStart = displayTime;
        lastAnchorSampleTime = displayTime;
        anchorValid = true;
        anchorInvalidated = false;
    }
    else if( mode == 2 && displayTime - lastAnchorSampleTime >= 1000000000LL )
    {
        const XrVector3f targetForward = RotateVector(targetAnchor.orientation, { 0, 0, -1 });
        const float targetYaw = std::atan2(targetForward.x, -targetForward.z);
        if( AngleDifference(candidateYaw, targetYaw) >= 15.0f * 3.14159265358979323846f / 180.0f )
        {
            anchorTransitionFrom = currentAnchor;
            targetAnchor = candidateAnchor;
            stationaryAnchor = candidateAnchor;
            anchorTransitionStart = displayTime;
        }
        lastAnchorSampleTime = displayTime;
    }

    if( mode == 1 )
    {
        currentAnchor = stationaryAnchor;
    }
    else
    {
        constexpr XrTime transitionDuration = 1000000000LL;
        const XrTime elapsed = displayTime > anchorTransitionStart ? displayTime - anchorTransitionStart : 0;
        const float amount = transitionDuration > 0
            ? static_cast<float>(elapsed) / static_cast<float>(transitionDuration)
            : 1.0f;
        currentAnchor = BlendPose(anchorTransitionFrom, targetAnchor, CubicEase(amount));
    }
    quadLayer.pose = currentAnchor;
}
void OpenXRPresenter::RecreateSwapchainForAspect(float aspect)
{
    if( session == XR_NULL_HANDLE || swapchain == XR_NULL_HANDLE || imageAcquired || aspect <= 0.001f ) return;
    if( swapchainAspect > 0.001f && std::fabs(swapchainAspect - aspect) < 0.01f ) return;

    constexpr uint32_t longSide = 2048;
    const uint32_t width = aspect >= 1.0f ? longSide : std::max(1u, static_cast<uint32_t>(std::lround(longSide * aspect)));
    const uint32_t height = aspect >= 1.0f ? std::max(1u, static_cast<uint32_t>(std::lround(longSide / aspect))) : longSide;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = swapchainFormat;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;

    XrSwapchain replacement = XR_NULL_HANDLE;
    if( createSwapchain(session, &info, &replacement) != XR_SUCCESS ) return;
    uint32_t count = 0;
    if( enumerateSwapchainImages(replacement, 0, &count, nullptr) != XR_SUCCESS || count == 0 )
    {
        destroySwapchain(replacement);
        return;
    }
    auto* images = new XrSwapchainImageVulkanKHR[count];
    for( uint32_t i = 0; i < count; ++i ) images[i] = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    if( enumerateSwapchainImages(replacement, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images)) != XR_SUCCESS )
    {
        delete[] images;
        destroySwapchain(replacement);
        return;
    }

    destroySwapchain(swapchain);
    delete[] swapchainImages;
    swapchain = replacement;
    swapchainImages = images;
    swapchainImageCount = count;
    swapchainExtent = {width, height};
    swapchainAspect = aspect;
    swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    quadLayer.subImage.swapchain = swapchain;
    quadLayer.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(width), static_cast<int32_t>(height)}};
    quadLayer.size.height = quadLayer.size.width / aspect;
}
bool OpenXRPresenter::BeginFrame()
{
    if (session == XR_NULL_HANDLE) return false;
    PollEvents();
    if (!sessionRunning) {
        if (sessionState != XR_SESSION_STATE_READY) return false;
        XrSessionBeginInfo sessionBeginInfo{XR_TYPE_SESSION_BEGIN_INFO};
        sessionBeginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (beginSession(session, &sessionBeginInfo) != XR_SUCCESS)
            Fail(RG_RESULT_OPENXR_SESSION_ERROR, "OpenXR session begin failed");
        sessionRunning = true;
    }
    if( lastSourceExtent.width != 0 && lastSourceExtent.height != 0 )
    {
        RecreateSwapchainForAspect(static_cast<float>(lastSourceExtent.width) / static_cast<float>(lastSourceExtent.height));
    }
    frameState = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    if (waitFrame(session, &waitInfo, &frameState) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_FRAME_ERROR, "OpenXR frame wait failed");
    UpdateQuadPose(frameState.predictedDisplayTime);
    XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (beginFrame(session, &frameBeginInfo) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_FRAME_ERROR, "OpenXR frame begin failed");
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo swapchainWaitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    swapchainWaitInfo.timeout = XR_INFINITE_DURATION;
    if (acquireSwapchainImage(swapchain, &acquireInfo, &swapchainImageIndex) != XR_SUCCESS ||
        waitSwapchainImage(swapchain, &swapchainWaitInfo) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_FRAME_ERROR, "OpenXR swapchain acquire failed");
    imageAcquired = true;
    frameActive = true;
    return true;
}

void OpenXRPresenter::RecordBlit(VkCommandBuffer cmd, VkImage source, VkExtent2D sourceExtent)
{
    if (!frameActive || !imageAcquired) return;
    lastSourceExtent = sourceExtent;
    VkImage destination = swapchainImages[swapchainImageIndex].image;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier barriers[2] = {};
    barriers[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, source, range};
    barriers[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        swapchainImageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, range};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<int32_t>(sourceExtent.width), static_cast<int32_t>(sourceExtent.height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<int32_t>(swapchainExtent.width), static_cast<int32_t>(swapchainExtent.height), 1};
    vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    swapchainImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void OpenXRPresenter::FinishFrame()
{
    if (!frameActive) return;
    if (imageAcquired) {
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (releaseSwapchainImage(swapchain, &releaseInfo) != XR_SUCCESS)
            Fail(RG_RESULT_OPENXR_PRESENTATION_ERROR, "OpenXR swapchain release failed");
    }
    imageAcquired = false;
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayer)};
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 1;
    endInfo.layers = layers;
    if (endFrame(session, &endInfo) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_PRESENTATION_ERROR, "OpenXR frame submission failed");
    frameActive = false;
}

void OpenXRPresenter::DestroySession()
{
    if (session != XR_NULL_HANDLE) {
        if (sessionRunning) endSession(session);
        if (headSpace != XR_NULL_HANDLE) destroySpace(headSpace);
        if (space != XR_NULL_HANDLE) destroySpace(space);
        if (swapchain != XR_NULL_HANDLE) destroySwapchain(swapchain);
        destroySession(session);
    }
    delete[] swapchainImages;
    swapchainImages = nullptr;
    session = XR_NULL_HANDLE;
    space = XR_NULL_HANDLE;
    headSpace = XR_NULL_HANDLE;
    swapchain = XR_NULL_HANDLE;
    anchorValid = false;
    anchorInvalidated = true;
}

}
#endif