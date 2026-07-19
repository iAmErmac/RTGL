#pragma once

#include <RTGL1/RTGL1.h>
#include <vulkan/vulkan.h>

#if defined(RG_WITH_OPENXR)
#define XR_USE_GRAPHICS_API_VULKAN
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <string>
#include <vector>

namespace RTGL1
{

class OpenXRPresenter
{
public:
    explicit OpenXRPresenter(const RgOpenXRPresentationCreateInfoEXT& info);
    ~OpenXRPresenter();

    OpenXRPresenter(const OpenXRPresenter&) = delete;
    OpenXRPresenter& operator=(const OpenXRPresenter&) = delete;

    const std::vector<const char*>& RequiredInstanceExtensions() const { return instanceExtensions; }
    const std::vector<const char*>& RequiredDeviceExtensions() const { return deviceExtensions; }
    VkPhysicalDevice GetPreferredPhysicalDevice(VkInstance instance) const;

    RgResult SetVirtualScreenSettings(const RgOpenXRVirtualScreenSettingsEXT& settings);
    void CreateSession(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                       VkQueue queue, uint32_t queueFamilyIndex);
    bool BeginFrame();
    void RecordBlit(VkCommandBuffer cmd, VkImage source, VkExtent2D sourceExtent);
    void FinishFrame();

    bool IsFrameActive() const { return frameActive; }

private:
    void LoadLoader();
    void LoadInstanceFunctions();
    void PollEvents();
    bool LocateHeadPose(XrTime displayTime, XrPosef& pose);
    void UpdateQuadPose(XrTime displayTime);
    void RecreateSwapchainForAspect(float aspect);
    void DestroySession();
    [[noreturn]] void Fail(RgResult result, const char* reason) const;

    RgBool32 desktopMirror;
    float quadWidthMeters;
    float quadDistanceMeters;

    void* loader = nullptr;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties enumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance createInstance = nullptr;
    PFN_xrDestroyInstance destroyInstance = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrResultToString resultToString = nullptr;
    PFN_xrGetVulkanInstanceExtensionsKHR getVulkanInstanceExtensions = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR getVulkanDeviceExtensions = nullptr;
    PFN_xrGetVulkanGraphicsRequirements2KHR getVulkanGraphicsRequirements2 = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getVulkanGraphicsDevice2 = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrLocateSpace locateSpace = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfigurationViews = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrAcquireSwapchainImage acquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage waitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseSwapchainImage = nullptr;

    XrInstance xrInstance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSpace headSpace = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrCompositionLayerQuad quadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrSwapchainImageVulkanKHR* swapchainImages = nullptr;
    uint32_t swapchainImageCount = 0;
    uint32_t swapchainImageIndex = 0;
    VkImageLayout swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    VkExtent2D lastSourceExtent{};
    float swapchainAspect = 0.0f;
    bool sessionRunning = false;
    bool frameActive = false;
    bool imageAcquired = false;
    RgOpenXRVirtualScreenSettingsEXT virtualScreenSettings{
        RG_STRUCTURE_TYPE_OPENXR_VIRTUAL_SCREEN_SETTINGS_EXT, nullptr, 2, RG_FALSE, 1.0f, 0.0f, 0.0f, 0, 0, 0
    };
    XrPosef stationaryAnchor{{0, 0, 0, 1}, {0, 0, 0}};
    XrPosef targetAnchor{{0, 0, 0, 1}, {0, 0, 0}};
    XrPosef currentAnchor{{0, 0, 0, 1}, {0, 0, 0}};
    XrTime lastAnchorSampleTime = 0;
    XrTime anchorTransitionStart = 0;
    XrPosef anchorTransitionFrom{{0, 0, 0, 1}, {0, 0, 0}};
    uint64_t consumedRecenterRequest = 0;
    int activeMode = -1;
    float activeDistance = -1.0f;
    float activeVerticalPosition = 0.0f;
    bool anchorValid = false;
    bool anchorInvalidated = true;

    std::vector<std::string> instanceExtensionStorage;
    std::vector<std::string> deviceExtensionStorage;
    std::vector<const char*> instanceExtensions;
    std::vector<const char*> deviceExtensions;
};

}
#endif