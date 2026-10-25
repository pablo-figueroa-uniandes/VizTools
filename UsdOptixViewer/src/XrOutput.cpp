#include "XrOutput.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <dlfcn.h>

namespace {

// Vulkan is loaded at run time from the driver, so the program needs no
// Vulkan SDK. There is one Vulkan device per process, so the function table
// can be global.
#define VULKAN_FUNCTIONS(X)                                                                                            \
    X(vkDestroyInstance)                                                                                               \
    X(vkGetPhysicalDeviceProperties2)                                                                                  \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    X(vkDestroyDevice)                                                                                                 \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkCmdPipelineBarrier)                                                                                            \
    X(vkCmdCopyBufferToImage)                                                                                          \
    X(vkQueueSubmit)                                                                                                   \
    X(vkCreateFence)                                                                                                   \
    X(vkDestroyFence)                                                                                                  \
    X(vkWaitForFences)                                                                                                 \
    X(vkResetFences)                                                                                                   \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkBindBufferMemory)                                                                                              \
    X(vkGetMemoryFdKHR)

struct Vulkan
{
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define DECLARE(name) PFN_##name name = nullptr;
    VULKAN_FUNCTIONS(DECLARE)
#undef DECLARE
} vk;

// The functions of the Vulkan binding are extensions, found through the instance.
struct
{
    PFN_xrGetVulkanGraphicsRequirements2KHR getGraphicsRequirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getGraphicsDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
} xrVulkan;

void xrCheck(XrInstance instance, XrResult result, const char* what)
{
    if (XR_SUCCEEDED(result))
        return;
    if (result == XR_ERROR_RUNTIME_UNAVAILABLE)  // before any instance exists, so not in xrResultToString
        throw std::runtime_error("no OpenXR runtime is available. Set XR_RUNTIME_JSON to a runtime's manifest, "
                                 "or use --cloudxr");
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, result, name)))
        snprintf(name, sizeof(name), "%d", int(result));
    throw std::runtime_error(std::string(what) + " failed: " + name);
}

void vkCheck(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " failed: VkResult " + std::to_string(int(result)));
}

void cudaCheck(cudaError_t result, const char* what)
{
    if (result != cudaSuccess)
        throw std::runtime_error(std::string(what) + " failed: " + cudaGetErrorString(result));
}

// q * v * q^-1 for a unit quaternion
XrVector3f rotate(const XrQuaternionf& q, XrVector3f v)
{
    const XrVector3f u{q.x, q.y, q.z};
    auto cross = [](XrVector3f a, XrVector3f b) {
        return XrVector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const XrVector3f c = cross(u, v);
    const XrVector3f t{c.x + q.w * v.x, c.y + q.w * v.y, c.z + q.w * v.z};
    const XrVector3f d = cross(u, t);
    return {v.x + 2.0f * d.x, v.y + 2.0f * d.y, v.z + 2.0f * d.z};
}

}  // namespace

XrOutput::~XrOutput()
{
    stop();
}

void XrOutput::stop()
{
    destroySession();
    destroyVulkan();
    if (m_instance != XR_NULL_HANDLE)
        xrDestroyInstance(m_instance);
    m_instance = XR_NULL_HANDLE;
    m_systemId = XR_NULL_SYSTEM_ID;
    m_state = State::Off;
}

// ---- instance and system ------------------------------------------------------
bool XrOutput::init(std::string& error)
{
    try
    {
        uint32_t count = 0;
        xrCheck(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
                "Looking for an OpenXR runtime");
        std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
        if (std::none_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
                return std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
            }))
            throw std::runtime_error("the OpenXR runtime does not support Vulkan (XR_KHR_vulkan_enable2)");

        const char* enabled[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strncpy(info.applicationInfo.applicationName, "UsdOptixViewer", XR_MAX_APPLICATION_NAME_SIZE - 1);
        std::strncpy(info.applicationInfo.engineName, "UsdOptixViewer", XR_MAX_ENGINE_NAME_SIZE - 1);
        info.applicationInfo.applicationVersion = 1;
        info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        info.enabledExtensionCount = 1;
        info.enabledExtensionNames = enabled;
        xrCheck(XR_NULL_HANDLE, xrCreateInstance(&info, &m_instance), "xrCreateInstance");

        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        xrCheck(m_instance, xrGetInstanceProperties(m_instance, &properties), "xrGetInstanceProperties");
        m_runtimeName = std::string(properties.runtimeName) + " " +
                        std::to_string(XR_VERSION_MAJOR(properties.runtimeVersion)) + "." +
                        std::to_string(XR_VERSION_MINOR(properties.runtimeVersion)) + "." +
                        std::to_string(XR_VERSION_PATCH(properties.runtimeVersion));

        auto load = [&](auto& fn, const char* name) {
            xrCheck(m_instance, xrGetInstanceProcAddr(m_instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn)),
                    name);
        };
        load(xrVulkan.getGraphicsRequirements, "xrGetVulkanGraphicsRequirements2KHR");
        load(xrVulkan.createInstance, "xrCreateVulkanInstanceKHR");
        load(xrVulkan.getGraphicsDevice, "xrGetVulkanGraphicsDevice2KHR");
        load(xrVulkan.createDevice, "xrCreateVulkanDeviceKHR");
        m_state = State::WaitingForSystem;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        stop();
        return false;
    }
    return true;
}

XrOutput::State XrOutput::update(std::string& error)
{
    try
    {
        if (m_state == State::WaitingForSystem)
            findSystem();
        else if (m_state == State::Idle || m_state == State::Running)
            pollEvents();
    }
    catch (const std::exception& e)
    {
        error = e.what();
        stop();
    }
    return m_state;
}

// A CloudXR "auto" device profile has no system until a client connects, so
// the system is looked for twice a second until it appears.
void XrOutput::findSystem()
{
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextSystemPoll)
        return;
    m_nextSystemPoll = now + std::chrono::milliseconds(500);

    XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult result = xrGetSystem(m_instance, &info, &m_systemId);
    if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
        return;
    xrCheck(m_instance, result, "xrGetSystem");
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    xrCheck(m_instance, xrGetSystemProperties(m_instance, m_systemId, &properties), "xrGetSystemProperties");
    m_systemName = properties.systemName;
    createVulkanDevice();
    m_state = State::SystemReady;
}

// ---- Vulkan -------------------------------------------------------------------
// The runtime creates the Vulkan instance and device itself (vulkan_enable2),
// adding the extensions it needs to ours, and tells us which GPU to use.
void XrOutput::createVulkanDevice()
{
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    xrCheck(m_instance, xrVulkan.getGraphicsRequirements(m_instance, m_systemId, &requirements),
            "xrGetVulkanGraphicsRequirements2KHR");
    const uint32_t apiVersion =
        std::max(VK_API_VERSION_1_1, VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                                                         XR_VERSION_MINOR(requirements.minApiVersionSupported), 0));

    m_vulkanLibrary = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!m_vulkanLibrary)
        throw std::runtime_error("libvulkan.so.1 not found");
    vk.vkGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m_vulkanLibrary, "vkGetInstanceProcAddr"));

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "UsdOptixViewer";
    app.apiVersion = apiVersion;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstanceInfo.systemId = m_systemId;
    xrInstanceInfo.pfnGetInstanceProcAddr = vk.vkGetInstanceProcAddr;
    xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
    VkResult vkResult = VK_SUCCESS;
    xrCheck(m_instance, xrVulkan.createInstance(m_instance, &xrInstanceInfo, &m_vkInstance, &vkResult),
            "xrCreateVulkanInstanceKHR");
    vkCheck(vkResult, "vkCreateInstance");
#define LOAD(name)                                                                                                     \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(m_vkInstance, #name));                             \
    if (!vk.name)                                                                                                      \
        throw std::runtime_error("Vulkan function " #name " not found");
    VULKAN_FUNCTIONS(LOAD)
#undef LOAD

    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGetInfo.systemId = m_systemId;
    deviceGetInfo.vulkanInstance = m_vkInstance;
    xrCheck(m_instance, xrVulkan.getGraphicsDevice(m_instance, &deviceGetInfo, &m_physicalDevice),
            "xrGetVulkanGraphicsDevice2KHR");
    VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids};
    vk.vkGetPhysicalDeviceProperties2(m_physicalDevice, &properties);
    std::copy(std::begin(ids.deviceUUID), std::end(ids.deviceUUID), m_deviceUuid.begin());
    m_gpuName = properties.properties.deviceName;

    uint32_t familyCount = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, families.data());
    const auto graphics = std::find_if(families.begin(), families.end(), [](const VkQueueFamilyProperties& f) {
        return f.queueFlags & VK_QUEUE_GRAPHICS_BIT;
    });
    if (graphics == families.end())
        throw std::runtime_error("the XR GPU has no graphics queue");
    m_queueFamily = uint32_t(graphics - families.begin());

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = m_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};  // memory shared with CUDA
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDeviceInfo.systemId = m_systemId;
    xrDeviceInfo.pfnGetInstanceProcAddr = vk.vkGetInstanceProcAddr;
    xrDeviceInfo.vulkanPhysicalDevice = m_physicalDevice;
    xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
    xrCheck(m_instance, xrVulkan.createDevice(m_instance, &xrDeviceInfo, &m_device, &vkResult),
            "xrCreateVulkanDeviceKHR");
    vkCheck(vkResult, "vkCreateDevice");
    vk.vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamily;
    vkCheck(vk.vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo bufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    bufferInfo.commandPool = m_commandPool;
    bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    bufferInfo.commandBufferCount = 1;
    vkCheck(vk.vkAllocateCommandBuffers(m_device, &bufferInfo, &m_commandBuffer), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCheck(vk.vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence), "vkCreateFence");
}

void XrOutput::destroyVulkan()
{
    if (m_device)
    {
        vk.vkDeviceWaitIdle(m_device);
        vk.vkDestroyFence(m_device, m_fence, nullptr);
        vk.vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        vk.vkDestroyDevice(m_device, nullptr);
    }
    if (m_vkInstance)
        vk.vkDestroyInstance(m_vkInstance, nullptr);
    if (m_vulkanLibrary)
        dlclose(m_vulkanLibrary);
    m_device = VK_NULL_HANDLE;
    m_fence = VK_NULL_HANDLE;
    m_commandPool = VK_NULL_HANDLE;
    m_commandBuffer = VK_NULL_HANDLE;
    m_queue = VK_NULL_HANDLE;
    m_physicalDevice = VK_NULL_HANDLE;
    m_vkInstance = VK_NULL_HANDLE;
    m_vulkanLibrary = nullptr;
}

// A device-local Vulkan buffer exported as a file descriptor and imported
// into CUDA: OptiX writes the eye's pixels into it, Vulkan copies them out.
void XrOutput::createSharedBuffer(Eye& eye)
{
    const VkDeviceSize size = VkDeviceSize(m_width) * m_height * sizeof(uchar4);
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vk.vkCreateBuffer(m_device, &bufferInfo, nullptr, &eye.buffer), "vkCreateBuffer");

    VkMemoryRequirements requirements{};
    vk.vkGetBufferMemoryRequirements(m_device, eye.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vk.vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memory);
    uint32_t type = 0;
    while (type < memory.memoryTypeCount &&
           !((requirements.memoryTypeBits & (1u << type)) &&
             (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
        ++type;
    if (type == memory.memoryTypeCount)
        throw std::runtime_error("no device-local Vulkan memory for the eye buffers");

    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.buffer = eye.buffer;
    VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated};
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo};
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = type;
    vkCheck(vk.vkAllocateMemory(m_device, &allocInfo, nullptr, &eye.memory), "vkAllocateMemory");
    vkCheck(vk.vkBindBufferMemory(m_device, eye.buffer, eye.memory, 0), "vkBindBufferMemory");

    VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    fdInfo.memory = eye.memory;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    vkCheck(vk.vkGetMemoryFdKHR(m_device, &fdInfo, &fd), "vkGetMemoryFdKHR");

    cudaExternalMemoryHandleDesc handle{};
    handle.type = cudaExternalMemoryHandleTypeOpaqueFd;
    handle.handle.fd = fd;  // owned by CUDA once imported
    handle.size = requirements.size;
    handle.flags = cudaExternalMemoryDedicated;
    cudaCheck(cudaImportExternalMemory(&eye.cudaMemory, &handle), "cudaImportExternalMemory");
    cudaExternalMemoryBufferDesc mapping{};
    mapping.size = size;
    void* pointer = nullptr;
    cudaCheck(cudaExternalMemoryGetMappedBuffer(&pointer, eye.cudaMemory, &mapping),
              "cudaExternalMemoryGetMappedBuffer");
    eye.pixels = static_cast<uchar4*>(pointer);
}

// ---- session ------------------------------------------------------------------
bool XrOutput::createSession(float resolutionScale, std::string& error)
{
    try
    {
        XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        binding.instance = m_vkInstance;
        binding.physicalDevice = m_physicalDevice;
        binding.device = m_device;
        binding.queueFamilyIndex = m_queueFamily;
        binding.queueIndex = 0;
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding};
        sessionInfo.systemId = m_systemId;
        xrCheck(m_instance, xrCreateSession(m_instance, &sessionInfo, &m_session), "xrCreateSession");

        // LOCAL space: the origin is where the head was when the session started
        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        xrCheck(m_instance, xrCreateReferenceSpace(m_session, &spaceInfo, &m_space), "xrCreateReferenceSpace");

        uint32_t count = 0;
        xrCheck(m_instance,
                xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                                 &count, nullptr),
                "xrEnumerateEnvironmentBlendModes");
        std::vector<XrEnvironmentBlendMode> blendModes(count);
        xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count,
                                         &count, blendModes.data());
        const bool opaque = blendModes.empty() ||
                            std::find(blendModes.begin(), blendModes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) !=
                                blendModes.end();
        m_blendMode = opaque ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : blendModes.front();

        xrCheck(m_instance,
                xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                  0, &count, nullptr),
                "xrEnumerateViewConfigurationViews");
        std::vector<XrViewConfigurationView> configViews(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count,
                                          &count, configViews.data());
        if (configViews.empty())
            throw std::runtime_error("the headset has no stereo views");
        const XrViewConfigurationView& view = configViews[0];
        m_width = std::clamp(unsigned(view.recommendedImageRectWidth * resolutionScale), 16u,
                             view.maxImageRectWidth);
        m_height = std::clamp(unsigned(view.recommendedImageRectHeight * resolutionScale), 16u,
                              view.maxImageRectHeight);

        // The renderer writes 8-bit gamma-encoded RGBA, which an sRGB format
        // shows as intended.
        xrCheck(m_instance, xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr), "xrEnumerateSwapchainFormats");
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(m_session, count, &count, formats.data());
        int64_t format = 0;
        for (int64_t wanted : {int64_t(VK_FORMAT_R8G8B8A8_SRGB), int64_t(VK_FORMAT_R8G8B8A8_UNORM)})
            if (!format && std::find(formats.begin(), formats.end(), wanted) != formats.end())
                format = wanted;
        if (!format)
            throw std::runtime_error("the runtime offers no RGBA8 swapchain format");

        m_eyes.resize(configViews.size());
        for (Eye& eye : m_eyes)
        {
            XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            swapchainInfo.format = format;
            swapchainInfo.sampleCount = 1;
            swapchainInfo.width = m_width;
            swapchainInfo.height = m_height;
            swapchainInfo.faceCount = 1;
            swapchainInfo.arraySize = 1;
            swapchainInfo.mipCount = 1;
            xrCheck(m_instance, xrCreateSwapchain(m_session, &swapchainInfo, &eye.swapchain), "xrCreateSwapchain");
            xrCheck(m_instance, xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr),
                    "xrEnumerateSwapchainImages");
            eye.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
            xrEnumerateSwapchainImages(eye.swapchain, count, &count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
            createSharedBuffer(eye);
        }
        m_views.assign(m_eyes.size(), {XR_TYPE_VIEW});
        m_state = State::Idle;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        stop();
        return false;
    }
    return true;
}

void XrOutput::destroySession()
{
    if (m_device)
        vk.vkDeviceWaitIdle(m_device);
    for (Eye& eye : m_eyes)
    {
        if (eye.pixels)
            cudaFree(eye.pixels);
        if (eye.cudaMemory)
            cudaDestroyExternalMemory(eye.cudaMemory);
        if (eye.buffer)
            vk.vkDestroyBuffer(m_device, eye.buffer, nullptr);
        if (eye.memory)
            vk.vkFreeMemory(m_device, eye.memory, nullptr);
        if (eye.swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(eye.swapchain);
    }
    m_eyes.clear();
    if (m_space != XR_NULL_HANDLE)
        xrDestroySpace(m_space);
    if (m_session != XR_NULL_HANDLE)
        xrDestroySession(m_session);
    m_space = XR_NULL_HANDLE;
    m_session = XR_NULL_HANDLE;
    m_frameState = {XR_TYPE_FRAME_STATE};
    m_viewsValid = false;
}

// The runtime drives the session through state changes: READY means "begin",
// STOPPING means "end". When the headset goes away (CloudXR client
// disconnected) the session is lost and we wait for a system again.
void XrOutput::pollEvents()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(m_instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
            throw std::runtime_error("the OpenXR runtime is shutting down");
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto& change = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            if (change.state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                xrCheck(m_instance, xrBeginSession(m_session, &beginInfo), "xrBeginSession");
                m_state = State::Running;
            }
            else if (change.state == XR_SESSION_STATE_STOPPING)
            {
                xrCheck(m_instance, xrEndSession(m_session), "xrEndSession");
                m_state = State::Idle;
            }
            else if (change.state == XR_SESSION_STATE_EXITING || change.state == XR_SESSION_STATE_LOSS_PENDING)
            {
                destroySession();
                destroyVulkan();
                m_systemId = XR_NULL_SYSTEM_ID;
                m_state = State::WaitingForSystem;
                return;
            }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---- frames -------------------------------------------------------------------
bool XrOutput::beginFrame(std::string& error)
{
    if (m_state != State::Running)
        return false;
    try
    {
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        m_frameState = {XR_TYPE_FRAME_STATE};
        xrCheck(m_instance, xrWaitFrame(m_session, &waitInfo, &m_frameState), "xrWaitFrame");  // paces the loop
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        xrCheck(m_instance, xrBeginFrame(m_session, &beginInfo), "xrBeginFrame");

        m_viewsValid = false;
        if (m_frameState.shouldRender)
        {
            XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime = m_frameState.predictedDisplayTime;
            locateInfo.space = m_space;
            XrViewState viewState{XR_TYPE_VIEW_STATE};
            uint32_t count = 0;
            xrCheck(m_instance,
                    xrLocateViews(m_session, &locateInfo, &viewState, uint32_t(m_views.size()), &count,
                                  m_views.data()),
                    "xrLocateViews");
            m_viewsValid = count == m_views.size() && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        }
    }
    catch (const std::exception& e)
    {
        error = e.what();
        stop();
        return false;
    }
    return true;
}

void XrOutput::endFrame(std::string& error)
{
    try
    {
        std::vector<XrCompositionLayerProjectionView> views;
        if (shouldRender())
        {
            copyToSwapchains();
            for (size_t i = 0; i < m_eyes.size(); ++i)
            {
                XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose = m_views[i].pose;
                view.fov = m_views[i].fov;
                view.subImage.swapchain = m_eyes[i].swapchain;
                view.subImage.imageRect.extent = {int32_t(m_width), int32_t(m_height)};
                views.push_back(view);
            }
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = m_space;
        layer.viewCount = uint32_t(views.size());
        layer.views = views.data();
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = m_frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = m_blendMode;
        endInfo.layerCount = views.empty() ? 0 : 1;
        endInfo.layers = layers;
        xrCheck(m_instance, xrEndFrame(m_session, &endInfo), "xrEndFrame");
    }
    catch (const std::exception& e)
    {
        error = e.what();
        stop();
    }
}

// The eye buffers were written by CUDA (the renderer synchronizes its stream
// before returning). Swapchain images arrive in COLOR_ATTACHMENT_OPTIMAL
// layout and must be returned in it.
void XrOutput::copyToSwapchains()
{
    std::vector<uint32_t> indices(m_eyes.size());
    for (size_t i = 0; i < m_eyes.size(); ++i)
    {
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        xrCheck(m_instance, xrAcquireSwapchainImage(m_eyes[i].swapchain, &acquireInfo, &indices[i]),
                "xrAcquireSwapchainImage");
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        xrCheck(m_instance, xrWaitSwapchainImage(m_eyes[i].swapchain, &waitInfo), "xrWaitSwapchainImage");
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vk.vkBeginCommandBuffer(m_commandBuffer, &beginInfo), "vkBeginCommandBuffer");
    for (size_t i = 0; i < m_eyes.size(); ++i)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_eyes[i].images[indices[i]].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // the whole image is overwritten
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vk.vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_width, m_height, 1};
        vk.vkCmdCopyBufferToImage(m_commandBuffer, m_eyes[i].buffer, barrier.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        vk.vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    vkCheck(vk.vkEndCommandBuffer(m_commandBuffer), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_commandBuffer;
    vkCheck(vk.vkQueueSubmit(m_queue, 1, &submit, m_fence), "vkQueueSubmit");
    // the eye buffers are rewritten next frame, so wait for the copy to finish
    vkCheck(vk.vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vkCheck(vk.vkResetFences(m_device, 1, &m_fence), "vkResetFences");

    for (Eye& eye : m_eyes)
    {
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrCheck(m_instance, xrReleaseSwapchainImage(eye.swapchain, &releaseInfo), "xrReleaseSwapchainImage");
    }
}

// ---- eye cameras ----------------------------------------------------------------
// The XR space is Y-up and looks along -Z, in meters. Its axes are mapped to
// the scene: +X to the anchor's right, +Y to the scene's up axis and -Z to the
// anchor's horizontal forward direction. Eye frusta are asymmetric, so w
// points at the centre of the image plane at distance 1 instead of straight
// ahead (the renderer accepts this because XR views use no clipping range).
Camera XrOutput::eyeCamera(unsigned int index, const Anchor& anchor) const
{
    const XrView& view = m_views[index];
    const float up[3] = {0.0f, anchor.zUp ? 0.0f : 1.0f, anchor.zUp ? 1.0f : 0.0f};
    float forward[3];
    const float along = anchor.forward[0] * up[0] + anchor.forward[1] * up[1] + anchor.forward[2] * up[2];
    for (int k = 0; k < 3; ++k)
        forward[k] = anchor.forward[k] - along * up[k];
    float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (length < 1e-6f)  // looking straight up or down: face the scene's -Y (Z-up) or -Z (Y-up)
    {
        forward[0] = 0.0f, forward[1] = anchor.zUp ? -1.0f : 0.0f, forward[2] = anchor.zUp ? 0.0f : -1.0f;
        length = 1.0f;
    }
    for (float& f : forward)
        f /= length;
    const float right[3] = {forward[1] * up[2] - forward[2] * up[1], forward[2] * up[0] - forward[0] * up[2],
                            forward[0] * up[1] - forward[1] * up[0]};
    auto toScene = [&](XrVector3f v) {
        return make_float3(right[0] * v.x + up[0] * v.y - forward[0] * v.z,
                           right[1] * v.x + up[1] * v.y - forward[1] * v.z,
                           right[2] * v.x + up[2] * v.y - forward[2] * v.z);
    };

    const XrQuaternionf& q = view.pose.orientation;
    const float3 eyeRight = toScene(rotate(q, {1.0f, 0.0f, 0.0f}));
    const float3 eyeUp = toScene(rotate(q, {0.0f, 1.0f, 0.0f}));
    const float3 eyeForward = toScene(rotate(q, {0.0f, 0.0f, -1.0f}));
    const float3 offset = toScene(view.pose.position);
    const float s = anchor.unitsPerMeter;

    const float tanLeft = std::tan(view.fov.angleLeft), tanRight = std::tan(view.fov.angleRight);
    const float tanDown = std::tan(view.fov.angleDown), tanUp = std::tan(view.fov.angleUp);
    const float cx = 0.5f * (tanRight + tanLeft), cy = 0.5f * (tanUp + tanDown);
    const float hx = 0.5f * (tanRight - tanLeft), hy = 0.5f * (tanUp - tanDown);
    Camera camera{};
    camera.eye = make_float3(anchor.eye[0] + offset.x * s, anchor.eye[1] + offset.y * s, anchor.eye[2] + offset.z * s);
    camera.u = make_float3(eyeRight.x * hx, eyeRight.y * hx, eyeRight.z * hx);
    camera.v = make_float3(eyeUp.x * hy, eyeUp.y * hy, eyeUp.z * hy);
    camera.w = make_float3(eyeForward.x + eyeRight.x * cx + eyeUp.x * cy, eyeForward.y + eyeRight.y * cx + eyeUp.y * cy,
                           eyeForward.z + eyeRight.z * cx + eyeUp.z * cy);
    camera.orthographic = 0;
    camera.clipNear = 0.0f;
    camera.clipFar = 1e16f;
    return camera;
}
