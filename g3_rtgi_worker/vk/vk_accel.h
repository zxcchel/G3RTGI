#pragma once
#include "vk_context.h"
#include "../../shared/ipc_protocol.h"
#include <cstdint>
#include <unordered_map>

struct VkBuf {
    VkBuffer        buf = VK_NULL_HANDLE;
    VkDeviceMemory  mem = VK_NULL_HANDLE;
    VkDeviceAddress addr = 0;
    VkDeviceSize    size = 0;
};

struct BlasEntry {
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkBuf asBuf;
    VkBuf vb;
    VkBuf ib;
    VkDeviceAddress asAddr = 0;
};

class VkAccel {
public:
    bool Init();
    void Shutdown();

    bool BuildBLAS(uint64_t meshId, const float* verts, uint32_t vCount,
        const uint32_t* indices, uint32_t iCount);
    bool BuildTLAS(const ipc::InstanceDesc* instances, uint32_t count);

    VkAccelerationStructureKHR GetTLAS() const { return m_tlas; }
    uint32_t GetBlasCount() const { return static_cast<uint32_t>(m_blasCache.size()); }
    uint32_t GetTlasInstanceCount() const { return m_tlasInstCount; }

private:
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags props, VkBuf& out);
    void DestroyBuffer(VkBuf& b);
    bool EnsureScratch(VkDeviceSize needed);
    void SubmitOneShot(VkCommandBuffer cmd);

    PFN_vkGetAccelerationStructureBuildSizesKHR    fpGetBuildSizes = nullptr;
    PFN_vkCreateAccelerationStructureKHR           fpCreateAS = nullptr;
    PFN_vkDestroyAccelerationStructureKHR          fpDestroyAS = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR        fpCmdBuildAS = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR fpGetASAddr = nullptr;

    VkCommandBuffer m_cmdBuf = VK_NULL_HANDLE;
    VkBuf           m_scratch;

    std::unordered_map<uint64_t, BlasEntry> m_blasCache;

    VkAccelerationStructureKHR m_tlas = VK_NULL_HANDLE;
    VkBuf                      m_tlasBuf;
    VkBuf                      m_instBuf;
    uint32_t                   m_tlasInstCount = 0;
};

extern VkAccel g_accel;