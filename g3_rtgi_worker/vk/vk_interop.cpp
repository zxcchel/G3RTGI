#include "vk_interop.h"
#include <string>
#include <cmath>

void LogLine(const std::string& s);

bool VkInteropSurface::Import(uint64_t sharedHandle, uint32_t width, uint32_t height) {
    if (m_image && m_handle == sharedHandle && m_width == width && m_height == height) {
        return true;
    }
    Destroy();
    if (!sharedHandle || width == 0 || height == 0) return false;

    VkExternalMemoryImageCreateInfo extImgInfo{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    extImgInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &extImgInfo;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = { width, height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult res = vkCreateImage(g_vk.device, &ici, nullptr, &m_image);
    if (res != VK_SUCCESS) {
        LogLine("[INTEROP][ERR] vkCreateImage failed, code=" + std::to_string((int)res));
        return false;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(g_vk.device, m_image, &memReq);

    VkMemoryDedicatedAllocateInfo dedInfo{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedInfo.image = m_image;

    VkImportMemoryWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    importInfo.pNext = &dedInfo;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    importInfo.handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sharedHandle));

    VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = g_vk.FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    res = vkAllocateMemory(g_vk.device, &allocInfo, nullptr, &m_memory);
    if (res != VK_SUCCESS) {
        LogLine("[INTEROP][ERR] vkAllocateMemory (KMT import) failed, code=" + std::to_string((int)res));
        Destroy();
        return false;
    }

    res = vkBindImageMemory(g_vk.device, m_image, m_memory, 0);
    if (res != VK_SUCCESS) {
        LogLine("[INTEROP][ERR] vkBindImageMemory failed, code=" + std::to_string((int)res));
        Destroy();
        return false;
    }
    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = m_image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(g_vk.device, &ivci, nullptr, &m_view) != VK_SUCCESS) {
        LogLine("[INTEROP][ERR] vkCreateImageView failed");
        Destroy();
        return false;
    }
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = g_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g_vk.device, &cbai, &m_cmdBuf);

    m_handle = sharedHandle;
    m_width = width;
    m_height = height;
    LogLine("[INTEROP] Imported D3D9Ex shared surface " + std::to_string(width) + "x" + std::to_string(height));
    return true;
}

void VkInteropSurface::RenderTestClear(uint32_t frameIndex) {
    if (!m_image || !m_cmdBuf) return;

    vkResetCommandBuffer(m_cmdBuf, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmdBuf, &bi);

    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier toDst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = m_image;
    toDst.subresourceRange = range;
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(m_cmdBuf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toDst);

    float t = frameIndex * 0.03f;
    VkClearColorValue color{};
    color.float32[0] = 0.5f + 0.5f * std::sin(t);
    color.float32[1] = 0.5f + 0.5f * std::sin(t + 2.094f);
    color.float32[2] = 0.5f + 0.5f * std::sin(t + 4.188f);
    color.float32[3] = 1.0f;

    vkCmdClearColorImage(m_cmdBuf, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

    VkImageMemoryBarrier toGen{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGen.image = m_image;
    toGen.subresourceRange = range;
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toGen.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

    vkCmdPipelineBarrier(m_cmdBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(m_cmdBuf);

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cmdBuf;
    vkQueueSubmit(g_vk.computeQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.computeQueue);
}

void VkInteropSurface::Destroy() {
    if (!g_vk.device) return;
    vkDeviceWaitIdle(g_vk.device);
    if (m_cmdBuf) { vkFreeCommandBuffers(g_vk.device, g_vk.cmdPool, 1, &m_cmdBuf); m_cmdBuf = VK_NULL_HANDLE; }
    if (m_view) { vkDestroyImageView(g_vk.device, m_view, nullptr); m_view = VK_NULL_HANDLE; }
    if (m_image) { vkDestroyImage(g_vk.device, m_image, nullptr);  m_image = VK_NULL_HANDLE; }
    if (m_memory) { vkFreeMemory(g_vk.device, m_memory, nullptr);   m_memory = VK_NULL_HANDLE; }
    m_handle = 0;
    m_width = 0;
    m_height = 0;
}