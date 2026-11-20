#pragma once
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <cstdint>

struct VkContext {
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    VkDevice         device = VK_NULL_HANDLE;
    VkQueue          computeQueue = VK_NULL_HANDLE;
    uint32_t         computeFamily = UINT32_MAX;
    VkCommandPool    cmdPool = VK_NULL_HANDLE;

    bool initialized = false;

    bool Init();
    void Shutdown();
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props) const;
};

extern VkContext g_vk;