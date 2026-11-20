#include "vk_rt_pipeline.h"
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>

void LogLine(const std::string& s);

VkRtPipeline g_rtPipe;

struct PushData {
    float    invViewProj[16];
    float    sunDir[4];
    float    sunColor[4];
    float    skyColor[4];
    uint32_t width;
    uint32_t height;
    uint32_t frameIndex;
    uint32_t pad;
};

struct DenoisePush {
    float invViewProj[16];
    float prevViewProj[16];
};

static bool InvertMat4(const float m[16], float invOut[16]) {
    double a[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) a[r][c] = m[r * 4 + c];
        a[r][r + 4] = 1.0;
    }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        if (std::fabs(a[piv][col]) < 1e-12) return false;
        if (piv != col)
            for (int k = 0; k < 8; ++k) std::swap(a[col][k], a[piv][k]);
        double div = a[col][col];
        for (int k = 0; k < 8; ++k) a[col][k] /= div;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            double f = a[r][col];
            for (int k = 0; k < 8; ++k) a[r][k] -= f * a[col][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            invOut[r * 4 + c] = static_cast<float>(a[r][c + 4]);
    return true;
}

static VkShaderModule LoadShader(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LogLine(std::string("[RT][ERR] Cannot open SPIR-V shader: ") + path);
        return VK_NULL_HANDLE;
    }
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), size);

    VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = size;
    smci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_vk.device, &smci, nullptr, &mod) != VK_SUCCESS) {
        LogLine(std::string("[RT][ERR] vkCreateShaderModule failed: ") + path);
        return VK_NULL_HANDLE;
    }
    return mod;
}

bool VkRtPipeline::CreateStorageImg(uint32_t w, uint32_t h, StorageImg& out) {
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_vk.device, &ici, nullptr, &out.img) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_vk.device, out.img, &req);

    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = g_vk.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_vk.device, &ai, nullptr, &out.mem) != VK_SUCCESS) return false;
    vkBindImageMemory(g_vk.device, out.img, out.mem, 0);

    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = out.img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return vkCreateImageView(g_vk.device, &ivci, nullptr, &out.view) == VK_SUCCESS;
}

void VkRtPipeline::DestroyStorageImg(StorageImg& s) {
    if (s.view) { vkDestroyImageView(g_vk.device, s.view, nullptr); s.view = VK_NULL_HANDLE; }
    if (s.img) { vkDestroyImage(g_vk.device, s.img, nullptr);      s.img = VK_NULL_HANDLE; }
    if (s.mem) { vkFreeMemory(g_vk.device, s.mem, nullptr);        s.mem = VK_NULL_HANDLE; }
}

void VkRtPipeline::DestroyInternalImages() {
    DestroyStorageImg(m_rawImg);
    DestroyStorageImg(m_histImg[0]);
    DestroyStorageImg(m_histImg[1]);
    m_imgW = m_imgH = 0;
    m_hasPrevVP = false;
}

bool VkRtPipeline::EnsureInternalImages(uint32_t w, uint32_t h) {
    if (m_imgW == w && m_imgH == h && m_rawImg.img) return true;
    vkDeviceWaitIdle(g_vk.device);
    DestroyInternalImages();
    if (!CreateStorageImg(w, h, m_rawImg) ||
        !CreateStorageImg(w, h, m_histImg[0]) ||
        !CreateStorageImg(w, h, m_histImg[1])) {
        DestroyInternalImages();
        return false;
    }

    // Переводим все 3 буфера в GENERAL и обнуляем историю
    vkResetCommandBuffer(m_cmdBuf, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmdBuf, &bi);

    VkImage imgs[3] = { m_rawImg.img, m_histImg[0].img, m_histImg[1].img };
    VkImageMemoryBarrier bars[3]{};
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for (int i = 0; i < 3; ++i) {
        bars[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bars[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bars[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        bars[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[i].image = imgs[i];
        bars[i].subresourceRange = range;
        bars[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(m_cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 3, bars);

    VkClearColorValue zero{};
    vkCmdClearColorImage(m_cmdBuf, m_histImg[0].img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    vkCmdClearColorImage(m_cmdBuf, m_histImg[1].img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);

    vkEndCommandBuffer(m_cmdBuf);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cmdBuf;
    vkQueueSubmit(g_vk.computeQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.computeQueue);

    m_imgW = w;
    m_imgH = h;
    return true;
}

bool VkRtPipeline::Init(const char* spvPath, const char* denoiseSpvPath) {
    m_shader = LoadShader(spvPath);
    m_denoiseShader = LoadShader(denoiseSpvPath);
    if (!m_shader || !m_denoiseShader) return false;

    // 1. RT Pipeline
    VkDescriptorSetLayoutBinding rtBinds[2]{};
    rtBinds[0] = { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    rtBinds[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 2;
    dslci.pBindings = rtBinds;
    vkCreateDescriptorSetLayout(g_vk.device, &dslci, nullptr, &m_dsLayout);

    VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushData) };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &m_dsLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(g_vk.device, &plci, nullptr, &m_pipeLayout);

    VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m_shader, "main", nullptr };
    cpci.layout = m_pipeLayout;
    if (vkCreateComputePipelines(g_vk.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_pipeline) != VK_SUCCESS) return false;

    // 2. Denoise Pipeline
    VkDescriptorSetLayoutBinding dnBinds[4]{};
    for (uint32_t i = 0; i < 4; ++i)
        dnBinds[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    dslci.bindingCount = 4;
    dslci.pBindings = dnBinds;
    vkCreateDescriptorSetLayout(g_vk.device, &dslci, nullptr, &m_denoiseDsLay);

    VkPushConstantRange dnPcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DenoisePush) };
    plci.pSetLayouts = &m_denoiseDsLay;
    plci.pPushConstantRanges = &dnPcr;
    vkCreatePipelineLayout(g_vk.device, &plci, nullptr, &m_denoiseLayout);

    cpci.stage.module = m_denoiseShader;
    cpci.layout = m_denoiseLayout;
    if (vkCreateComputePipelines(g_vk.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_denoisePipe) != VK_SUCCESS) return false;

    // 3. Descriptors & CmdBuf
    VkDescriptorPoolSize poolSizes[2]{
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 },
    };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 2;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(g_vk.device, &dpci, nullptr, &m_dsPool);

    VkDescriptorSetLayout layouts[2] = { m_dsLayout, m_denoiseDsLay };
    VkDescriptorSet sets[2]{};
    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = m_dsPool;
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = layouts;
    vkAllocateDescriptorSets(g_vk.device, &dsai, sets);
    m_ds = sets[0];
    m_denoiseDs = sets[1];

    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = g_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g_vk.device, &cbai, &m_cmdBuf);

    LogLine("[RT] RayQuery + Spatial/Temporal Denoiser initialized!");
    return true;
}

void VkRtPipeline::Dispatch(VkAccelerationStructureKHR tlas, VkImage outImg, VkImageView outView,
    const ipc::FrameHeader& hdr)
{
    if (!m_pipeline || !m_denoisePipe || !tlas || !outImg || !outView) return;
    if (!EnsureInternalImages(hdr.width, hdr.height)) return;

    PushData pc{};
    if (!InvertMat4(hdr.viewProj, pc.invViewProj)) return;
    memcpy(pc.sunDir, hdr.sunDir, sizeof(pc.sunDir));
    memcpy(pc.sunColor, hdr.sunColor, sizeof(pc.sunColor));
    memcpy(pc.skyColor, hdr.skyColor, sizeof(pc.skyColor));
    pc.width = hdr.width;
    pc.height = hdr.height;
    pc.frameIndex = hdr.frameIndex;

    DenoisePush dnPc{};
    memcpy(dnPc.invViewProj, pc.invViewProj, sizeof(dnPc.invViewProj));
    memcpy(dnPc.prevViewProj, m_hasPrevVP ? m_prevViewProj : hdr.viewProj, sizeof(dnPc.prevViewProj));

    int readIdx = m_histIdx;
    int writeIdx = 1 - m_histIdx;

    // Обновляем дескрипторы RT и Denoise
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &tlas;

    VkDescriptorImageInfo rawInfo{ VK_NULL_HANDLE, m_rawImg.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo hrInfo{ VK_NULL_HANDLE, m_histImg[readIdx].view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo hwInfo{ VK_NULL_HANDLE, m_histImg[writeIdx].view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, outView, VK_IMAGE_LAYOUT_GENERAL };

    VkWriteDescriptorSet writes[6]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite, m_ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  m_ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &rawInfo, nullptr, nullptr };
    writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  m_denoiseDs, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &rawInfo, nullptr, nullptr };
    writes[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  m_denoiseDs, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hrInfo,  nullptr, nullptr };
    writes[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  m_denoiseDs, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hwInfo,  nullptr, nullptr };
    writes[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  m_denoiseDs, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo, nullptr, nullptr };
    vkUpdateDescriptorSets(g_vk.device, 6, writes, 0, nullptr);

    vkResetCommandBuffer(m_cmdBuf, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmdBuf, &bi);

    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    uint32_t gx = (hdr.width + 7) / 8;
    uint32_t gy = (hdr.height + 7) / 8;

    // Pass 1: RayQuery GI -> m_rawImg
    vkCmdBindPipeline(m_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(m_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout, 0, 1, &m_ds, 0, nullptr);
    vkCmdPushConstants(m_cmdBuf, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushData), &pc);
    vkCmdDispatch(m_cmdBuf, gx, gy, 1);

    // Барьер: m_rawImg готов для чтения, outImg переводим в GENERAL
    VkImageMemoryBarrier midBars[2]{};
    midBars[0] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, m_rawImg.img, range };
    midBars[1] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, outImg, range };
    vkCmdPipelineBarrier(m_cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, midBars);

    // Pass 2: Denoiser -> m_histImg[writeIdx] + outImg
    vkCmdBindPipeline(m_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoisePipe);
    vkCmdBindDescriptorSets(m_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoiseLayout, 0, 1, &m_denoiseDs, 0, nullptr);
    vkCmdPushConstants(m_cmdBuf, m_denoiseLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DenoisePush), &dnPc);
    vkCmdDispatch(m_cmdBuf, gx, gy, 1);

    VkImageMemoryBarrier doneBar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, outImg, range };
    vkCmdPipelineBarrier(m_cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &doneBar);

    vkEndCommandBuffer(m_cmdBuf);

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cmdBuf;
    vkQueueSubmit(g_vk.computeQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.computeQueue);

    m_histIdx = writeIdx;
    memcpy(m_prevViewProj, hdr.viewProj, sizeof(m_prevViewProj));
    m_hasPrevVP = true;
}

void VkRtPipeline::Shutdown() {
    if (!g_vk.device) return;
    vkDeviceWaitIdle(g_vk.device);
    DestroyInternalImages();
    if (m_cmdBuf) { vkFreeCommandBuffers(g_vk.device, g_vk.cmdPool, 1, &m_cmdBuf);           m_cmdBuf = VK_NULL_HANDLE; }
    if (m_dsPool) { vkDestroyDescriptorPool(g_vk.device, m_dsPool, nullptr);                 m_dsPool = VK_NULL_HANDLE; }
    if (m_denoisePipe) { vkDestroyPipeline(g_vk.device, m_denoisePipe, nullptr);                  m_denoisePipe = VK_NULL_HANDLE; }
    if (m_denoiseLayout) { vkDestroyPipelineLayout(g_vk.device, m_denoiseLayout, nullptr);          m_denoiseLayout = VK_NULL_HANDLE; }
    if (m_denoiseDsLay) { vkDestroyDescriptorSetLayout(g_vk.device, m_denoiseDsLay, nullptr);      m_denoiseDsLay = VK_NULL_HANDLE; }
    if (m_denoiseShader) { vkDestroyShaderModule(g_vk.device, m_denoiseShader, nullptr);            m_denoiseShader = VK_NULL_HANDLE; }
    if (m_pipeline) { vkDestroyPipeline(g_vk.device, m_pipeline, nullptr);                     m_pipeline = VK_NULL_HANDLE; }
    if (m_pipeLayout) { vkDestroyPipelineLayout(g_vk.device, m_pipeLayout, nullptr);             m_pipeLayout = VK_NULL_HANDLE; }
    if (m_dsLayout) { vkDestroyDescriptorSetLayout(g_vk.device, m_dsLayout, nullptr);          m_dsLayout = VK_NULL_HANDLE; }
    if (m_shader) { vkDestroyShaderModule(g_vk.device, m_shader, nullptr);                   m_shader = VK_NULL_HANDLE; }
}