#pragma once
#include "vk_context.h"
#include <cstdint>

class VkInteropSurface {
public:
    VkImage     GetImage() const { return m_image; }
    VkImageView GetView()  const { return m_view; }
    bool Import(uint64_t sharedHandle, uint32_t width, uint32_t height);
    void RenderTestClear(uint32_t frameIndex);
    void Destroy();

    uint64_t CurrentHandle() const { return m_handle; }
    bool IsValid() const { return m_image != VK_NULL_HANDLE; }

private:
    
    uint64_t        m_handle = 0;
    uint32_t        m_width = 0;
    uint32_t        m_height = 0;
    VkImage         m_image = VK_NULL_HANDLE;
    VkDeviceMemory  m_memory = VK_NULL_HANDLE;
    VkCommandBuffer m_cmdBuf = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
};