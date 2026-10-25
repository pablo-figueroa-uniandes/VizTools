// Stereo output through OpenXR, for example to a headset streamed by NVIDIA
// CloudXR. The runtime chooses the GPU and creates the Vulkan device. Each
// eye has a buffer in Vulkan memory that is shared with CUDA, so OptiX renders
// straight into it; a copy on the Vulkan queue moves it into the eye's
// swapchain image.
#pragma once

#include <array>
#include <chrono>
#include <string>
#include <vector>

#include <cuda_runtime.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "LaunchParams.h"

class XrOutput
{
public:
    enum class State
    {
        Off,               // no OpenXR instance
        WaitingForSystem,  // no headset yet (a CloudXR "auto" profile waits for a client)
        SystemReady,       // Vulkan device created; the app must call createSession()
        Idle,              // session created, not running
        Running,           // frames are being submitted
    };

    // Places the user in the scene: the origin of the XR space (the head's
    // position when the session started) goes to `eye`, facing the
    // horizontal part of `forward`, and one meter spans unitsPerMeter units.
    struct Anchor
    {
        float eye[3];
        float forward[3];
        bool zUp;
        float unitsPerMeter;
    };

    XrOutput() = default;
    ~XrOutput();
    XrOutput(const XrOutput&) = delete;
    XrOutput& operator=(const XrOutput&) = delete;

    // Creates the OpenXR instance with the active runtime (XR_RUNTIME_JSON).
    bool init(std::string& error);
    // Ends the session and releases everything (State::Off).
    void stop();
    // Handles runtime events and looks for the headset. On an error the XR
    // output is shut down (State::Off) and `error` is set.
    State update(std::string& error);
    State state() const { return m_state; }
    // In State::SystemReady: CUDA must be current on the GPU with this UUID
    // before createSession(), which shares memory between Vulkan and CUDA.
    const std::array<unsigned char, 16>& deviceUuid() const { return m_deviceUuid; }
    bool createSession(float resolutionScale, std::string& error);

    // One frame: when beginFrame() returns true, render each eye into
    // eyeBuffer() if shouldRender(), then call endFrame().
    bool beginFrame(std::string& error);
    bool shouldRender() const { return m_frameState.shouldRender && m_viewsValid; }
    void endFrame(std::string& error);

    unsigned int eyeCount() const { return unsigned(m_eyes.size()); }
    unsigned int eyeWidth() const { return m_width; }
    unsigned int eyeHeight() const { return m_height; }
    uchar4* eyeBuffer(unsigned int eye) const { return m_eyes[eye].pixels; }
    Camera eyeCamera(unsigned int eye, const Anchor& anchor) const;

    const std::string& runtimeName() const { return m_runtimeName; }
    const std::string& systemName() const { return m_systemName; }
    const std::string& gpuName() const { return m_gpuName; }

private:
    struct Eye
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageVulkan2KHR> images;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        cudaExternalMemory_t cudaMemory = nullptr;
        uchar4* pixels = nullptr;  // the buffer, mapped into CUDA
    };

    void findSystem();
    void createVulkanDevice();
    void createSharedBuffer(Eye& eye);
    void pollEvents();
    void copyToSwapchains();
    void destroySession();
    void destroyVulkan();

    State m_state = State::Off;
    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_space = XR_NULL_HANDLE;
    XrEnvironmentBlendMode m_blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrFrameState m_frameState{XR_TYPE_FRAME_STATE};
    std::vector<XrView> m_views;
    bool m_viewsValid = false;
    std::chrono::steady_clock::time_point m_nextSystemPoll;
    std::string m_runtimeName, m_systemName, m_gpuName;

    void* m_vulkanLibrary = nullptr;
    VkInstance m_vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    std::array<unsigned char, 16> m_deviceUuid{};

    std::vector<Eye> m_eyes;
    unsigned int m_width = 0, m_height = 0;
};
