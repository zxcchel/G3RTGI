#pragma once
#include "vk_context.h"
#include "../../shared/ipc_protocol.h"

struct StorageImg {
    VkImage        img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
};

class VkRtPipeline {
public:
    bool Init(const char* spvPath, const char* denoiseSpvPath = "denoise.spv");
    void Shutdown();
    void Dispatch(VkAccelerationStructureKHR tlas, VkImage outImg, VkImageView outView,
        const ipc::FrameHeader& hdr);

private:
    bool EnsureInternalImages(uint32_t w, uint32_t h);
    void DestroyInternalImages();
    bool CreateStorageImg(uint32_t w, uint32_t h, StorageImg& out);
    void DestroyStorageImg(StorageImg& s);

    VkShaderModule        m_shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dsLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline = VK_NULL_HANDLE;

    VkShaderModule        m_denoiseShader = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_denoiseDsLay = VK_NULL_HANDLE;
    VkPipelineLayout      m_denoiseLayout = VK_NULL_HANDLE;
    VkPipeline            m_denoisePipe = VK_NULL_HANDLE;

    VkDescriptorPool      m_dsPool = VK_NULL_HANDLE;
    VkDescriptorSet       m_ds = VK_NULL_HANDLE;
    VkDescriptorSet       m_denoiseDs = VK_NULL_HANDLE;
    VkCommandBuffer       m_cmdBuf = VK_NULL_HANDLE;

    StorageImg m_rawImg{};
    StorageImg m_histImg[2]{};
    uint32_t   m_imgW = 0, m_imgH = 0;
    int        m_histIdx = 0;
    float      m_prevViewProj[16]{};
    bool       m_hasPrevVP = false;
};

extern VkRtPipeline g_rtPipe;