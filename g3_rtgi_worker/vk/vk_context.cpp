#include "vk_context.h"
#include <vector>
#include <string>
#include <cstring>
#include <iterator>

// Реализация LogLine находится в worker/main.cpp
void LogLine(const std::string& s);

VkContext g_vk;

bool VkContext::Init() {
    if (initialized) return true;

    VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "G3_RTGI_Worker";
    appInfo.apiVersion = VK_API_VERSION_1_3;

    const char* instExts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = static_cast<uint32_t>(std::size(instExts));
    ici.ppEnabledExtensionNames = instExts;

    VkResult res = vkCreateInstance(&ici, nullptr, &instance);
    if (res != VK_SUCCESS) {
        LogLine("[VK][ERR] vkCreateInstance failed, code=" + std::to_string((int)res));
        return false;
    }
    LogLine("[VK] Instance created");

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LogLine("[VK][ERR] No Vulkan GPUs found");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());

    for (auto gpu : gpus) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physDevice = gpu;
            LogLine(std::string("[VK] Selected GPU: ") + props.deviceName);
            break;
        }
    }
    if (!physDevice) physDevice = gpus[0];

    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qProps(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &qCount, qProps.data());
    for (uint32_t i = 0; i < qCount; ++i) {
        if ((qProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            (qProps[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
            computeFamily = i;
            break;
        }
    }
    if (computeFamily == UINT32_MAX) {
        LogLine("[VK][ERR] No compute queue family found");
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = computeFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* devExts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    };

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availExts(extCount);
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &extCount, availExts.data());

    for (const char* req : devExts) {
        bool found = false;
        for (const auto& e : availExts) {
            if (strcmp(e.extensionName, req) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            LogLine(std::string("[VK][MISSING EXT] ") + req);
        }
    }

    VkPhysicalDeviceRayQueryFeaturesKHR rqFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    rqFeat.rayQuery = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    asFeat.accelerationStructure = VK_TRUE;
    asFeat.pNext = &rqFeat;

    VkPhysicalDeviceVulkan12Features v12Feat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    v12Feat.bufferDeviceAddress = VK_TRUE;
    v12Feat.pNext = &asFeat;

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &v12Feat;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(std::size(devExts));
    dci.ppEnabledExtensionNames = devExts;

    res = vkCreateDevice(physDevice, &dci, nullptr, &device);
    if (res != VK_SUCCESS) {
        LogLine("[VK][ERR] vkCreateDevice failed, code=" + std::to_string((int)res));
        return false;
    }

    vkGetDeviceQueue(device, computeFamily, 0, &computeQueue);

    VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = computeFamily;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(device, &cpci, nullptr, &cmdPool) != VK_SUCCESS) {
        LogLine("[VK][ERR] vkCreateCommandPool failed");
        return false;
    }

    initialized = true;
    LogLine("[VK] 64-bit Vulkan 1.3 + RayQuery + Win32 Interop initialized!");
    return true;
}

void VkContext::Shutdown() {
    if (!initialized) return;
    vkDeviceWaitIdle(device);
    if (cmdPool)  vkDestroyCommandPool(device, cmdPool, nullptr);
    if (device)   vkDestroyDevice(device, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    cmdPool = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    initialized = false;
}

uint32_t VkContext::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}