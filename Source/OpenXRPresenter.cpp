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
RgResult OpenXRPresenter::SetMenuPointerBeam(const RgOpenXRMenuPointerBeamEXT& beam)
{
    if( beam.sType != RG_STRUCTURE_TYPE_OPENXR_MENU_POINTER_BEAM_EXT ) return RG_RESULT_WRONG_STRUCTURE_TYPE;
    menuPointerBeam = beam;
    menuPointerBeam.length = std::max(0.0f, beam.length);
    return RG_RESULT_SUCCESS;
}
RgResult OpenXRPresenter::SetPresentationSettings(const RgOpenXRPresentationSettingsEXT& settings)
{
    if( settings.structSize != sizeof(settings) || settings.version != RG_OPENXR_PRESENTATION_EXT_VERSION ) return RG_RESULT_WRONG_FUNCTION_ARGUMENT;
    if( settings.presentationMode != RG_OPENXR_PRESENTATION_MODE_VIRTUAL_SCREEN_EXT && settings.presentationMode != RG_OPENXR_PRESENTATION_MODE_STEREO_PROJECTION_EXT ) return RG_RESULT_WRONG_FUNCTION_ARGUMENT;
    if( settings.mirrorMode < RG_OPENXR_MIRROR_MODE_LEFT_EYE_EXT || settings.mirrorMode > RG_OPENXR_MIRROR_MODE_OFF_EXT || settings.eyeRenderScale <= 0.0f ) return RG_RESULT_WRONG_FUNCTION_ARGUMENT;
    if( settings.requestSerial == acceptedPresentationSerial ) return RG_RESULT_SUCCESS;
    requestedPresentationSettings = settings;
    if( settings.presentationMode == RG_OPENXR_PRESENTATION_MODE_STEREO_PROJECTION_EXT )
        RecreateProjectionSwapchain();
    acceptedPresentationSerial = settings.requestSerial;
    return RG_RESULT_SUCCESS;
}
RgResult OpenXRPresenter::ApplyHapticFeedback(uint32_t hand, float durationSeconds, float amplitude)
{
    if (!applyHapticFeedback || !stopHapticFeedback || session == XR_NULL_HANDLE ||
        hapticAction == XR_NULL_HANDLE || !sessionRunning || hand > 1)
        return RG_RESULT_OPENXR_SESSION_ERROR;
    XrHapticActionInfo actionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
    actionInfo.action = hapticAction;
    actionInfo.subactionPath = hand == 0 ? leftHandPath : rightHandPath;
    if (durationSeconds <= 0.0f || amplitude <= 0.0f)
        return stopHapticFeedback(session, &actionInfo) == XR_SUCCESS ? RG_RESULT_SUCCESS : RG_RESULT_OPENXR_SESSION_ERROR;
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.duration = static_cast<XrDuration>(std::clamp(durationSeconds, 0.001f, 10.0f) * 1000000000.0f);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    return applyHapticFeedback(session, &actionInfo, reinterpret_cast<const XrHapticBaseHeader*>(&vibration)) == XR_SUCCESS
        ? RG_RESULT_SUCCESS
        : RG_RESULT_OPENXR_SESSION_ERROR;
}
RgResult OpenXRPresenter::GetFrameState(RgOpenXRFrameStateEXT& state) const
{
    state = xrFrameState;
    return RG_RESULT_SUCCESS;
}
void OpenXRPresenter::InitializeActions()
{
    if (!createActionSet || !createAction || !attachActionSets || !stringToPath) return;
    stringToPath(xrInstance, "/user/hand/left", &leftHandPath);
    stringToPath(xrInstance, "/user/hand/right", &rightHandPath);
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "doomxrt");
    std::strcpy(setInfo.localizedActionSetName, "DoomXRT");
    if (createActionSet(xrInstance, &setInfo, &actionSet) != XR_SUCCESS) return;
    const XrPath handPaths[] = {leftHandPath, rightHandPath};
    auto make = [&](const char* name, XrActionType type, XrAction& out, XrAction& mirror) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = type;
        std::strcpy(info.actionName, name);
        std::strcpy(info.localizedActionName, name);
        info.countSubactionPaths = 2;
        info.subactionPaths = handPaths;
        if (createAction(actionSet, &info, &out) != XR_SUCCESS) return false;
        mirror = out;
        return true;
    };
    // A single action with both hand paths is required by several OpenXR runtimes.
    make("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, leftStick, rightStick);
    make("pose", XR_ACTION_TYPE_POSE_INPUT, leftPoseAction, rightPoseAction);
    make("trigger", XR_ACTION_TYPE_FLOAT_INPUT, leftTrigger, rightTrigger);
    make("grip", XR_ACTION_TYPE_FLOAT_INPUT, leftGrip, rightGrip);
    make("grip_click", XR_ACTION_TYPE_BOOLEAN_INPUT, leftGripClick, rightGripClick);
    make("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, leftMenu, rightMenu);
    make("face_x", XR_ACTION_TYPE_BOOLEAN_INPUT, leftFaceX, rightFaceX);
    make("face_y", XR_ACTION_TYPE_BOOLEAN_INPUT, leftFaceY, rightFaceY);
    make("thumb_click", XR_ACTION_TYPE_BOOLEAN_INPUT, leftThumbClick, rightThumbClick);
    make("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, hapticAction, hapticAction);
    XrPath touchProfile = XR_NULL_PATH;
    stringToPath(xrInstance, "/interaction_profiles/oculus/touch_controller", &touchProfile);
    auto path = [&](const char* value) { XrPath result = XR_NULL_PATH; stringToPath(xrInstance, value, &result); return result; };
    XrActionSuggestedBinding bindings[] = {
        {leftStick, path("/user/hand/left/input/thumbstick")}, {rightStick, path("/user/hand/right/input/thumbstick")},
        {leftPoseAction, path("/user/hand/left/input/aim/pose")}, {rightPoseAction, path("/user/hand/right/input/aim/pose")},
        {leftTrigger, path("/user/hand/left/input/trigger/value")}, {rightTrigger, path("/user/hand/right/input/trigger/value")},
        {leftGrip, path("/user/hand/left/input/squeeze/value")}, {rightGrip, path("/user/hand/right/input/squeeze/value")},
        {leftGripClick, path("/user/hand/left/input/squeeze/click")}, {rightGripClick, path("/user/hand/right/input/squeeze/click")},
        {leftMenu, path("/user/hand/left/input/menu/click")},
        {leftFaceX, path("/user/hand/left/input/x/click")}, {rightFaceX, path("/user/hand/right/input/a/click")},
        {leftFaceY, path("/user/hand/left/input/y/click")}, {rightFaceY, path("/user/hand/right/input/b/click")},
        {leftThumbClick, path("/user/hand/left/input/thumbstick/click")}, {rightThumbClick, path("/user/hand/right/input/thumbstick/click")},
        {hapticAction, path("/user/hand/left/output/haptic")}, {hapticAction, path("/user/hand/right/output/haptic")},

    };
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = touchProfile;
    suggested.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);
    suggested.suggestedBindings = bindings;
    if (suggestBindings) suggestBindings(xrInstance, &suggested);
    auto suggestProfile = [&](const char* profile, const std::initializer_list<std::pair<XrAction, const char*>>& entries) {
        XrPath profilePath = XR_NULL_PATH; stringToPath(xrInstance, profile, &profilePath);
        std::vector<XrActionSuggestedBinding> profileBindings;
        for (const auto& entry : entries) profileBindings.push_back({entry.first, path(entry.second)});
        XrInteractionProfileSuggestedBinding profileSuggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        profileSuggested.interactionProfile = profilePath;
        profileSuggested.countSuggestedBindings = static_cast<uint32_t>(profileBindings.size());
        profileSuggested.suggestedBindings = profileBindings.data();
        suggestBindings(xrInstance, &profileSuggested);
    };
    // Quest 3 / Touch Plus is exposed as its own interaction profile by Meta runtimes.
    suggestProfile("/interaction_profiles/meta/touch_plus_controller", {
        {leftStick, "/user/hand/left/input/thumbstick"}, {rightStick, "/user/hand/right/input/thumbstick"},
        {leftTrigger, "/user/hand/left/input/trigger/value"}, {rightTrigger, "/user/hand/right/input/trigger/value"},
        {leftGrip, "/user/hand/left/input/squeeze/value"}, {rightGrip, "/user/hand/right/input/squeeze/value"},
        {leftGripClick, "/user/hand/left/input/squeeze/click"}, {rightGripClick, "/user/hand/right/input/squeeze/click"},
        {leftMenu, "/user/hand/left/input/menu/click"},
        {leftFaceX, "/user/hand/left/input/x/click"}, {rightFaceX, "/user/hand/right/input/a/click"},
        {leftFaceY, "/user/hand/left/input/y/click"}, {rightFaceY, "/user/hand/right/input/b/click"},
        {leftThumbClick, "/user/hand/left/input/thumbstick/click"}, {rightThumbClick, "/user/hand/right/input/thumbstick/click"},
        {leftPoseAction, "/user/hand/left/input/aim/pose"}, {rightPoseAction, "/user/hand/right/input/aim/pose"},
        {hapticAction, "/user/hand/left/output/haptic"}, {hapticAction, "/user/hand/right/output/haptic"}
    });
    suggestProfile("/interaction_profiles/valve/index_controller", {
        {leftStick, "/user/hand/left/input/thumbstick"}, {rightStick, "/user/hand/right/input/thumbstick"},
        {leftTrigger, "/user/hand/left/input/trigger/value"}, {rightTrigger, "/user/hand/right/input/trigger/value"},
        {leftGrip, "/user/hand/left/input/squeeze/value"}, {rightGrip, "/user/hand/right/input/squeeze/value"},
        {leftGripClick, "/user/hand/left/input/squeeze/click"}, {rightGripClick, "/user/hand/right/input/squeeze/click"},

        {leftThumbClick, "/user/hand/left/input/thumbstick/click"}, {rightThumbClick, "/user/hand/right/input/thumbstick/click"},
        {leftPoseAction, "/user/hand/left/input/aim/pose"}, {rightPoseAction, "/user/hand/right/input/aim/pose"},
        {hapticAction, "/user/hand/left/output/haptic"}, {hapticAction, "/user/hand/right/output/haptic"}
    });
    suggestProfile("/interaction_profiles/microsoft/motion_controller", {
        {leftStick, "/user/hand/left/input/thumbstick"}, {rightStick, "/user/hand/right/input/thumbstick"},
        {leftTrigger, "/user/hand/left/input/trigger/value"}, {rightTrigger, "/user/hand/right/input/trigger/value"},
        {leftGrip, "/user/hand/left/input/squeeze/value"}, {rightGrip, "/user/hand/right/input/squeeze/value"},
        {leftGripClick, "/user/hand/left/input/squeeze/click"}, {rightGripClick, "/user/hand/right/input/squeeze/click"},

        {leftPoseAction, "/user/hand/left/input/aim/pose"}, {rightPoseAction, "/user/hand/right/input/aim/pose"},
        {hapticAction, "/user/hand/left/output/haptic"}, {hapticAction, "/user/hand/right/output/haptic"}
    });

}

void OpenXRPresenter::AttachActions()
{
    if (session == XR_NULL_HANDLE || actionSet == XR_NULL_HANDLE || !attachActionSets)
        Fail(RG_RESULT_OPENXR_SESSION_ERROR, "OpenXR input action setup is unavailable");

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet;
    if (attachActionSets(session, &attach) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_SESSION_ERROR, "OpenXR action-set attachment failed");
    if (!createActionSpace)
        Fail(RG_RESULT_OPENXR_SESSION_ERROR, "OpenXR action-space creation is unavailable");

    XrActionSpaceCreateInfo leftInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    leftInfo.action = leftPoseAction;
    leftInfo.subactionPath = leftHandPath;
    leftInfo.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    if (createActionSpace(session, &leftInfo, &leftHandSpace) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_SESSION_ERROR, "OpenXR left-hand action-space creation failed");

    XrActionSpaceCreateInfo rightInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    rightInfo.action = rightPoseAction;
    rightInfo.subactionPath = rightHandPath;
    rightInfo.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    if (createActionSpace(session, &rightInfo, &rightHandSpace) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_SESSION_ERROR, "OpenXR right-hand action-space creation failed");
}

void OpenXRPresenter::SyncInputActions(RgOpenXRInputSnapshotEXT& snapshot)
{
    if (!syncActions || actionSet == XR_NULL_HANDLE || !sessionRunning) return;
    XrActiveActionSet active{actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (syncActions(session, &sync) != XR_SUCCESS) return;

    auto vec = [&](XrAction action, XrPath hand, RgOpenXRControllerStateEXT& out) {
        if (!action) return;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO}; get.action = action; get.subactionPath = hand;
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (getActionStateVector2f(session, &get, &state) == XR_SUCCESS && state.isActive) {
            out.stick.data[0] = state.currentState.x; out.stick.data[1] = state.currentState.y;
            out.tracked = RG_TRUE;
        }
    };
    auto scalar = [&](XrAction action, XrPath hand, float& out) {
        if (!action) return;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO}; get.action = action; get.subactionPath = hand;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (getActionStateFloat(session, &get, &state) == XR_SUCCESS && state.isActive) out = state.currentState;
    };
    auto button = [&](XrAction action, XrPath hand, RgBool32& out) {
        if (!action) return;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO}; get.action = action; get.subactionPath = hand;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (getActionStateBoolean(session, &get, &state) == XR_SUCCESS && state.isActive) out = state.currentState ? RG_TRUE : RG_FALSE;
    };
    vec(leftStick, leftHandPath, snapshot.left); vec(rightStick, rightHandPath, snapshot.right);
    scalar(leftTrigger, leftHandPath, snapshot.left.trigger); scalar(rightTrigger, rightHandPath, snapshot.right.trigger);
    scalar(leftGrip, leftHandPath, snapshot.left.grip); scalar(rightGrip, rightHandPath, snapshot.right.grip);
    auto gripClick = [&](XrAction action, XrPath hand, float& out) {
        if (!action) return;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO}; get.action = action; get.subactionPath = hand;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (getActionStateBoolean(session, &get, &state) == XR_SUCCESS && state.isActive && state.currentState)
            out = 1.0f;
    };
    gripClick(leftGripClick, leftHandPath, snapshot.left.grip); gripClick(rightGripClick, rightHandPath, snapshot.right.grip);
    button(leftMenu, leftHandPath, snapshot.left.menu); button(rightMenu, rightHandPath, snapshot.right.menu);
    button(leftFaceX, leftHandPath, snapshot.left.faceX); button(rightFaceX, rightHandPath, snapshot.right.faceA);
    button(leftFaceY, leftHandPath, snapshot.left.faceY); button(rightFaceY, rightHandPath, snapshot.right.faceB);
    button(leftThumbClick, leftHandPath, snapshot.left.thumbClick); button(rightThumbClick, rightHandPath, snapshot.right.thumbClick);
    auto pose = [&](XrSpace handSpace, RgOpenXRPoseEXT& out) {
        if (!locateSpace || handSpace == XR_NULL_HANDLE || space == XR_NULL_HANDLE) return;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (locateSpace(handSpace, space, frameState.predictedDisplayTime, &location) == XR_SUCCESS &&
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            out.position.data[0] = location.pose.position.x; out.position.data[1] = location.pose.position.y; out.position.data[2] = location.pose.position.z;
            out.orientation.data[0] = location.pose.orientation.x; out.orientation.data[1] = location.pose.orientation.y; out.orientation.data[2] = location.pose.orientation.z; out.orientation.data[3] = location.pose.orientation.w;
            out.valid = RG_TRUE;
        }
    };
    pose(leftHandSpace, snapshot.left.pose); pose(rightHandSpace, snapshot.right.pose);
    snapshot.virtualScreenPose.position.data[0] = quadLayer.pose.position.x; snapshot.virtualScreenPose.position.data[1] = quadLayer.pose.position.y; snapshot.virtualScreenPose.position.data[2] = quadLayer.pose.position.z;
    snapshot.virtualScreenPose.orientation.data[0] = quadLayer.pose.orientation.x; snapshot.virtualScreenPose.orientation.data[1] = quadLayer.pose.orientation.y; snapshot.virtualScreenPose.orientation.data[2] = quadLayer.pose.orientation.z; snapshot.virtualScreenPose.orientation.data[3] = quadLayer.pose.orientation.w; snapshot.virtualScreenPose.valid = RG_TRUE;
    XrPosef headPose{};
    if( LocateHeadPose(frameState.predictedDisplayTime, headPose) )
    {
        snapshot.headPose.position.data[0] = headPose.position.x; snapshot.headPose.position.data[1] = headPose.position.y; snapshot.headPose.position.data[2] = headPose.position.z;
        snapshot.headPose.orientation.data[0] = headPose.orientation.x; snapshot.headPose.orientation.data[1] = headPose.orientation.y; snapshot.headPose.orientation.data[2] = headPose.orientation.z; snapshot.headPose.orientation.data[3] = headPose.orientation.w; snapshot.headPose.valid = RG_TRUE;
    }
    snapshot.virtualScreenSize.data[0] = quadLayer.size.width; snapshot.virtualScreenSize.data[1] = quadLayer.size.height; snapshot.virtualScreenRevision = consumedRecenterRequest;
    snapshot.capabilities = RG_OPENXR_INPUT_CAPABILITY_INTERACTION_PROFILE | RG_OPENXR_INPUT_CAPABILITY_LEFT | RG_OPENXR_INPUT_CAPABILITY_RIGHT;
}
RgResult OpenXRPresenter::GetInputSnapshot(RgOpenXRInputSnapshotEXT& snapshot) const
{
    snapshot = inputSnapshot;
    snapshot.structSize = sizeof(snapshot);
    snapshot.version = RG_OPENXR_INPUT_SNAPSHOT_EXT_VERSION;
    snapshot.sessionRunning = sessionRunning ? RG_TRUE : RG_FALSE;
    snapshot.focused = sessionState == XR_SESSION_STATE_FOCUSED ? RG_TRUE : RG_FALSE;
    snapshot.frameTime = static_cast<int64_t>(frameState.predictedDisplayTime);
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
    LOAD_XR(xrLocateViews, locateViews);
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
    LOAD_XR(xrStringToPath, stringToPath);
    LOAD_XR(xrCreateActionSet, createActionSet);
    LOAD_XR(xrDestroyActionSet, destroyActionSet);
    LOAD_XR(xrCreateAction, createAction);
    LOAD_XR(xrDestroyAction, destroyAction);
    LOAD_XR(xrSuggestInteractionProfileBindings, suggestBindings);
    LOAD_XR(xrAttachSessionActionSets, attachActionSets);
    LOAD_XR(xrSyncActions, syncActions);
    LOAD_XR(xrGetActionStateFloat, getActionStateFloat);
    LOAD_XR(xrGetActionStateVector2f, getActionStateVector2f);
    LOAD_XR(xrGetActionStateBoolean, getActionStateBoolean);
    LOAD_XR(xrCreateActionSpace, createActionSpace);
    LOAD_XR(xrApplyHapticFeedback, applyHapticFeedback);
    LOAD_XR(xrStopHapticFeedback, stopHapticFeedback);
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
    InitializeActions();

    if (createSession(xrInstance, &sessionInfo, &session) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_SESSION_ERROR, "OpenXR Vulkan session creation failed");
    AttachActions();

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

    projectionRecommendedExtent = swapchainExtent;

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
bool OpenXRPresenter::CreateMenuPointerBeamSwapchain()
{
    if( session == XR_NULL_HANDLE || swapchainFormat == VK_FORMAT_UNDEFINED ) return false;
    if( menuPointerBeamSwapchain != XR_NULL_HANDLE ) return true;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = swapchainFormat;
    info.sampleCount = 1;
    info.width = 8;
    info.height = 8;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if( createSwapchain(session, &info, &menuPointerBeamSwapchain) != XR_SUCCESS ) return false;
    if( enumerateSwapchainImages(menuPointerBeamSwapchain, 0, &menuPointerBeamSwapchainImageCount, nullptr) != XR_SUCCESS || menuPointerBeamSwapchainImageCount == 0 )
    {
        DestroyMenuPointerBeamSwapchain();
        return false;
    }
    menuPointerBeamSwapchainImages = new XrSwapchainImageVulkanKHR[menuPointerBeamSwapchainImageCount];
    for( uint32_t i = 0; i < menuPointerBeamSwapchainImageCount; ++i ) menuPointerBeamSwapchainImages[i] = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    if( enumerateSwapchainImages(menuPointerBeamSwapchain, menuPointerBeamSwapchainImageCount, &menuPointerBeamSwapchainImageCount,
                                 reinterpret_cast<XrSwapchainImageBaseHeader*>(menuPointerBeamSwapchainImages)) != XR_SUCCESS )
    {
        DestroyMenuPointerBeamSwapchain();
        return false;
    }
    return true;
}

void OpenXRPresenter::DestroyMenuPointerBeamSwapchain()
{
    if( menuPointerBeamSwapchain != XR_NULL_HANDLE ) destroySwapchain(menuPointerBeamSwapchain);
    delete[] menuPointerBeamSwapchainImages;
    menuPointerBeamSwapchain = XR_NULL_HANDLE;
    menuPointerBeamSwapchainImages = nullptr;
    menuPointerBeamSwapchainImageCount = 0;
    menuPointerBeamSwapchainImageIndex = 0;
    menuPointerBeamSwapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void OpenXRPresenter::DestroyProjectionSwapchain()
{
    if( projectionSwapchain != XR_NULL_HANDLE ) destroySwapchain( projectionSwapchain );
    delete[] projectionSwapchainImages;
    projectionSwapchain = XR_NULL_HANDLE;
    projectionSwapchainImages = nullptr;
    projectionSwapchainImageCount = 0;
    projectionExtent = {};
    projectionRenderScale = 0.0f;
}

void OpenXRPresenter::RecreateProjectionSwapchain()
{
    if( session == XR_NULL_HANDLE || imageAcquired || projectionRecommendedExtent.width == 0 || projectionRecommendedExtent.height == 0 ) return;

    const float scale = std::clamp( requestedPresentationSettings.eyeRenderScale, 0.25f, 2.0f );
    const VkExtent2D desired{
        std::max( 1u, static_cast<uint32_t>( std::lround( projectionRecommendedExtent.width * scale ) ) ),
        std::max( 1u, static_cast<uint32_t>( std::lround( projectionRecommendedExtent.height * scale ) ) ),
    };
    if( projectionSwapchain != XR_NULL_HANDLE && projectionExtent.width == desired.width &&
        projectionExtent.height == desired.height && std::fabs( projectionRenderScale - scale ) < 0.0001f ) return;

    DestroyProjectionSwapchain();
    XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = swapchainFormat;
    info.sampleCount = 1;
    info.width = desired.width;
    info.height = desired.height;
    info.faceCount = 1;
    info.arraySize = 2;
    info.mipCount = 1;
    if( createSwapchain( session, &info, &projectionSwapchain ) != XR_SUCCESS )
    {
        projectionSwapchain = XR_NULL_HANDLE;
        return;
    }
    if( enumerateSwapchainImages( projectionSwapchain, 0, &projectionSwapchainImageCount, nullptr ) != XR_SUCCESS || projectionSwapchainImageCount == 0 )
    {
        DestroyProjectionSwapchain();
        return;
    }
    projectionSwapchainImages = new XrSwapchainImageVulkanKHR[ projectionSwapchainImageCount ];
    for( uint32_t i = 0; i < projectionSwapchainImageCount; ++i ) projectionSwapchainImages[ i ] = { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR };
    if( enumerateSwapchainImages( projectionSwapchain, projectionSwapchainImageCount, &projectionSwapchainImageCount,
                                  reinterpret_cast<XrSwapchainImageBaseHeader*>( projectionSwapchainImages ) ) != XR_SUCCESS )
    {
        DestroyProjectionSwapchain();
        return;
    }
    projectionExtent = desired;
    projectionRenderScale = scale;
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
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    viewLocateInfo.displayTime = frameState.predictedDisplayTime;
    viewLocateInfo.space = space;
    uint32_t viewCount = 0;
    if( locateViews && locateViews(session, &viewLocateInfo, &viewState, 2, &viewCount, locatedViews) == XR_SUCCESS && viewCount >= 2 )
    {
        for( uint32_t eye = 0; eye < 2; ++eye )
        {
            const auto& view = locatedViews[eye];
            auto& out = xrFrameState.eyes[eye];
            const XrSpaceLocationFlags validFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if( (viewState.viewStateFlags & validFlags) != validFlags ) continue;
            out.pose.position = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
            out.pose.orientation = {{view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z, view.pose.orientation.w}};
            out.pose.valid = RG_TRUE;
            out.fieldOfView[0] = view.fov.angleLeft; out.fieldOfView[1] = view.fov.angleRight; out.fieldOfView[2] = view.fov.angleUp; out.fieldOfView[3] = view.fov.angleDown;
            const float fovAdjustment = std::clamp(requestedPresentationSettings.fovAdjustment * 0.01745329252f, -0.5f, 0.5f);
            const XrFovf adjustedFov = {
                std::clamp(view.fov.angleLeft - fovAdjustment, -1.569f, 1.569f),
                std::clamp(view.fov.angleRight + fovAdjustment, -1.569f, 1.569f),
                std::clamp(view.fov.angleUp + fovAdjustment, -1.569f, 1.569f),
                std::clamp(view.fov.angleDown - fovAdjustment, -1.569f, 1.569f),
            };
            projectionFovs[ eye ] = adjustedFov;
            const float l = std::tan(adjustedFov.angleLeft);
            const float r = std::tan(adjustedFov.angleRight);
            const float u = std::tan(adjustedFov.angleUp);
            const float d = std::tan(adjustedFov.angleDown);
            const float n = 0.01f, f = 1000.0f;
            std::fill(std::begin(out.projection), std::end(out.projection), 0.0f);
            out.projection[0] = 2.0f / (r - l); out.projection[5] = -2.0f / (u - d); out.projection[8] = (r + l) / (r - l); out.projection[9] = -(u + d) / (u - d); out.projection[10] = f / (n - f); out.projection[11] = -1.0f; out.projection[14] = (f * n) / (n - f);
        }
    }
    XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    shouldRender = frameState.shouldRender == XR_TRUE;
    if (beginFrame(session, &frameBeginInfo) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_FRAME_ERROR, "OpenXR frame begin failed");

    frameActive = true;
    inputSnapshot = {};
    inputSnapshot.structSize = sizeof(inputSnapshot);
    inputSnapshot.version = RG_OPENXR_INPUT_SNAPSHOT_EXT_VERSION;
    inputSnapshot.sessionRunning = RG_TRUE;
    inputSnapshot.focused = sessionState == XR_SESSION_STATE_FOCUSED ? RG_TRUE : RG_FALSE;
    inputSnapshot.frameTime = static_cast<int64_t>(frameState.predictedDisplayTime);
    SyncInputActions(inputSnapshot);

    xrFrameState.capabilities = RG_OPENXR_PRESENTATION_CAPABILITY_VIRTUAL_SCREEN_EXT;
    if( viewCount >= 2 ) xrFrameState.capabilities |= RG_OPENXR_PRESENTATION_CAPABILITY_STEREO_PROJECTION_EXT;
    xrFrameState.sessionRunning = sessionRunning;
    xrFrameState.focused = sessionState == XR_SESSION_STATE_FOCUSED;
    xrFrameState.frameValid = RG_TRUE;
    xrFrameState.predictedDisplayTime = static_cast<int64_t>(frameState.predictedDisplayTime);
    XrPosef headPose{};
    if( LocateHeadPose(frameState.predictedDisplayTime, headPose) ) {
        xrFrameState.headPose.position = {headPose.position.x, headPose.position.y, headPose.position.z};
        xrFrameState.headPose.orientation = {{headPose.orientation.x, headPose.orientation.y, headPose.orientation.z, headPose.orientation.w}};
        xrFrameState.headPose.valid = RG_TRUE;
    }
    xrFrameState.requestedPresentationMode = requestedPresentationSettings.presentationMode;
    xrFrameState.requestedMirrorMode = requestedPresentationSettings.mirrorMode;
    const bool projectionReady = requestedPresentationSettings.presentationMode == RG_OPENXR_PRESENTATION_MODE_STEREO_PROJECTION_EXT &&
        projectionSwapchain != XR_NULL_HANDLE && projectionSwapchainImageCount > 0 && viewCount >= 2;
    xrFrameState.activePresentationMode = projectionReady
        ? RG_OPENXR_PRESENTATION_MODE_STEREO_PROJECTION_EXT
        : RG_OPENXR_PRESENTATION_MODE_VIRTUAL_SCREEN_EXT;
    xrFrameState.activeMirrorMode = projectionReady ? requestedPresentationSettings.mirrorMode : RG_OPENXR_MIRROR_MODE_OFF_EXT;
    xrFrameState.fallbackReason = projectionReady || requestedPresentationSettings.presentationMode == RG_OPENXR_PRESENTATION_MODE_VIRTUAL_SCREEN_EXT
        ? RG_OPENXR_PRESENTATION_FALLBACK_NONE_EXT : RG_OPENXR_PRESENTATION_FALLBACK_NOT_READY_EXT;
    return true;
}

void OpenXRPresenter::RecordBlit(VkCommandBuffer cmd, VkImage source, VkExtent2D sourceExtent)
{
    if (!frameActive) return;
    if (!imageAcquired) {
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (acquireSwapchainImage(swapchain, &acquireInfo, &swapchainImageIndex) != XR_SUCCESS ||
            waitSwapchainImage(swapchain, &waitInfo) != XR_SUCCESS)
            Fail(RG_RESULT_OPENXR_FRAME_ERROR, "OpenXR quad swapchain acquire failed");
        imageAcquired = true;
        quadImageAcquired = true;
        activeSwapchain = swapchain;
    }
    if (activeSwapchain != swapchain) return;
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
    RecordMenuPointerBeam(cmd);
}

void OpenXRPresenter::RecordMenuPointerBeam(VkCommandBuffer cmd)
{
    if( !menuPointerBeam.visible || !frameActive || !shouldRender || !CreateMenuPointerBeamSwapchain() ) return;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    if( acquireSwapchainImage(menuPointerBeamSwapchain, &acquireInfo, &menuPointerBeamSwapchainImageIndex) != XR_SUCCESS ||
        waitSwapchainImage(menuPointerBeamSwapchain, &waitInfo) != XR_SUCCESS ) return;
    menuPointerBeamImageAcquired = true;
    const VkImage destination = menuPointerBeamSwapchainImages[menuPointerBeamSwapchainImageIndex].image;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        menuPointerBeamSwapchainImageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, range};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkClearColorValue color{};
    color.float32[0] = menuPointerBeam.color.data[0]; color.float32[1] = menuPointerBeam.color.data[1];
    color.float32[2] = menuPointerBeam.color.data[2]; color.float32[3] = menuPointerBeam.color.data[3];
    vkCmdClearColorImage(cmd, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    menuPointerBeamSwapchainImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    menuPointerBeamLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    menuPointerBeamLayer.space = space;
    menuPointerBeamLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    menuPointerBeamLayer.pose = {{menuPointerBeam.pose.orientation.data[0], menuPointerBeam.pose.orientation.data[1], menuPointerBeam.pose.orientation.data[2], menuPointerBeam.pose.orientation.data[3]},
                                  {menuPointerBeam.pose.position.data[0], menuPointerBeam.pose.position.data[1], menuPointerBeam.pose.position.data[2]}};
    menuPointerBeamLayer.size = {std::max(0.02f, menuPointerBeam.length), 0.005f};
    menuPointerBeamLayer.subImage.swapchain = menuPointerBeamSwapchain;
    menuPointerBeamLayer.subImage.imageRect = {{0, 0}, {8, 8}};
    menuPointerBeamSubmitted = true;
}

void OpenXRPresenter::RecordHudBlit(VkCommandBuffer cmd, VkImage source, VkExtent2D sourceExtent)
{
    if( !frameActive || !shouldRender ) return;
    RecordBlit( cmd, source, sourceExtent );
    if( !quadImageAcquired ) return;
    XrPosef headPose{};
    if( !LocateHeadPose( frameState.predictedDisplayTime, headPose ) ) return;
    hudLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    hudLayer.space = space;
    hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    hudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    const XrVector3f offset = RotateVector( headPose.orientation, { 0, 0, -1.0f } );
    hudLayer.pose = { NormalizeQuaternion( headPose.orientation ), { headPose.position.x + offset.x, headPose.position.y + offset.y - 0.30f, headPose.position.z + offset.z } };
    hudLayer.size = { 1.0f, sourceExtent.width > 0 ? float( sourceExtent.height ) / float( sourceExtent.width ) : 1.0f };
    hudLayer.subImage.swapchain = swapchain;
    hudLayer.subImage.imageRect = {{ 0, 0 }, { int32_t( swapchainExtent.width ), int32_t( swapchainExtent.height ) }};
    hudSubmitted = true;
}
void OpenXRPresenter::RecordStereoEyeBlit(VkCommandBuffer cmd, uint32_t eyeIndex, VkImage source, VkExtent2D sourceExtent)
{
    if( !frameActive || !shouldRender || projectionSwapchain == XR_NULL_HANDLE || eyeIndex > 1 ) return;
    if( eyeIndex == 0 )
    {
        if( projectionImageAcquired ) return;
        XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        if( acquireSwapchainImage( projectionSwapchain, &acquireInfo, &projectionSwapchainImageIndex ) != XR_SUCCESS ||
            waitSwapchainImage( projectionSwapchain, &waitInfo ) != XR_SUCCESS )
            Fail( RG_RESULT_OPENXR_FRAME_ERROR, "OpenXR projection swapchain acquire failed" );
        projectionImageAcquired = true;
        activeSwapchain = projectionSwapchain;
    }
    if( !projectionImageAcquired || activeSwapchain != projectionSwapchain ) return;

    const uint32_t physicalEye = requestedPresentationSettings.swapEyes ? 1u - eyeIndex : eyeIndex;
    VkImage destination = projectionSwapchainImages[ projectionSwapchainImageIndex ].image;
    VkImageSubresourceRange srcRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange dstRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, physicalEye, 1 };
    VkImageMemoryBarrier barriers[ 2 ] = {};
    barriers[ 0 ] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, source, srcRange };
    barriers[ 1 ] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        projectionSwapchainImageLayouts[ physicalEye ], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, dstRange };
    vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers );

    const VkClearColorValue black{};
    vkCmdClearColorImage( cmd, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &dstRange );
    VkImageBlit region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[ 1 ] = { int32_t( sourceExtent.width ), int32_t( sourceExtent.height ), 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, physicalEye, 1 };
    region.dstOffsets[ 0 ] = { 0, 0, 0 };
    region.dstOffsets[ 1 ] = { int32_t( projectionExtent.width ), int32_t( projectionExtent.height ), 1 };
    vkCmdBlitImage( cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR );
    barriers[ 0 ].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barriers[ 0 ].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[ 0 ].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barriers[ 0 ].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[ 1 ].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barriers[ 1 ].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[ 1 ].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barriers[ 1 ].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers );
    projectionSwapchainImageLayouts[ physicalEye ] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if( eyeIndex == 1 )
    {
        projectionLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION }; projectionLayer.space = space; projectionLayer.viewCount = 2; projectionLayer.views = projectionViews;
        for( uint32_t eye = 0; eye < 2; ++eye ) { projectionViews[ eye ] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }; projectionViews[ eye ].pose = locatedViews[ eye ].pose; projectionViews[ eye ].fov = projectionFovs[ eye ]; projectionViews[ eye ].subImage.swapchain = projectionSwapchain; projectionViews[ eye ].subImage.imageArrayIndex = eye; projectionViews[ eye ].subImage.imageRect = {{ 0, 0 }, { int32_t( projectionExtent.width ), int32_t( projectionExtent.height ) }}; }
        projectionSubmitted = true;
    }
}
void OpenXRPresenter::FinishFrame()
{
    if (!frameActive) return;
    XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if( projectionImageAcquired && releaseSwapchainImage( projectionSwapchain, &releaseInfo ) != XR_SUCCESS )
        Fail( RG_RESULT_OPENXR_PRESENTATION_ERROR, "OpenXR projection swapchain release failed" );
    if( quadImageAcquired && releaseSwapchainImage( swapchain, &releaseInfo ) != XR_SUCCESS )
        Fail( RG_RESULT_OPENXR_PRESENTATION_ERROR, "OpenXR quad swapchain release failed" );
    if( menuPointerBeamImageAcquired && releaseSwapchainImage( menuPointerBeamSwapchain, &releaseInfo ) != XR_SUCCESS )
        Fail( RG_RESULT_OPENXR_PRESENTATION_ERROR, "OpenXR menu pointer beam swapchain release failed" );
    const XrCompositionLayerBaseHeader* layers[ 3 ]{};
    uint32_t layerCount = 0;
    if( projectionSubmitted ) layers[ layerCount++ ] = reinterpret_cast< const XrCompositionLayerBaseHeader* >( &projectionLayer );
    if( hudSubmitted ) layers[ layerCount++ ] = reinterpret_cast< const XrCompositionLayerBaseHeader* >( &hudLayer );
    else if( quadSubmitted || ( !projectionSubmitted && quadImageAcquired ) ) layers[ layerCount++ ] = reinterpret_cast< const XrCompositionLayerBaseHeader* >( &quadLayer );
    if( menuPointerBeamSubmitted ) layers[ layerCount++ ] = reinterpret_cast< const XrCompositionLayerBaseHeader* >( &menuPointerBeamLayer );
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount > 0 ? layers : nullptr;
    if (endFrame(session, &endInfo) != XR_SUCCESS)
        Fail(RG_RESULT_OPENXR_PRESENTATION_ERROR, "OpenXR frame submission failed");
    imageAcquired = false;
    quadImageAcquired = false;
    projectionImageAcquired = false;
    quadSubmitted = false;
    hudSubmitted = false;
    activeSwapchain = XR_NULL_HANDLE;
    projectionSubmitted = false;
    menuPointerBeamImageAcquired = false;
    menuPointerBeamSubmitted = false;
    quadLayer.space = space;
    quadLayer.layerFlags = 0;
    frameActive = false;
}
void OpenXRPresenter::DestroySession()
{
    if (session != XR_NULL_HANDLE) {
        if (sessionRunning) endSession(session);
        if (headSpace != XR_NULL_HANDLE) destroySpace(headSpace);
        if (space != XR_NULL_HANDLE) destroySpace(space);
        if (swapchain != XR_NULL_HANDLE) destroySwapchain(swapchain);
        DestroyProjectionSwapchain();
        DestroyMenuPointerBeamSwapchain();
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