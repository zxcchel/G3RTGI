#include "vk_accel.h"
#include <vector>
#include <string>
#include <cstring>

void LogLine(const std::string& s);

VkAccel g_accel;

bool VkAccel::Init() {
    fpGetBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(g_vk.device, "vkGetAccelerationStructureBuildSizesKHR"));
    fpCreateAS = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(g_vk.device, "vkCreateAccelerationStructureKHR"));
    fpDestroyAS = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(g_vk.device, "vkDestroyAccelerationStructureKHR"));
    fpCmdBuildAS = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(g_vk.device, "vkCmdBuildAccelerationStructuresKHR"));
    fpGetASAddr = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(g_vk.device, "vkGetAccelerationStructureDeviceAddressKHR"));

    if (!fpGetBuildSizes || !fpCreateAS || !fpDestroyAS || !fpCmdBuildAS || !fpGetASAddr) {
        LogLine("[ACCEL][ERR] Failed to load KHR_acceleration_structure function pointers");
        return false;
    }

    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = g_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_vk.device, &cbai, &m_cmdBuf) != VK_SUCCESS) {
        LogLine("[ACCEL][ERR] vkAllocateCommandBuffers failed");
        return false;
    }

    // Постоянный буфер под инстансы TLAS (HOST_VISIBLE для мгновенного обновления каждый кадр)
    VkDeviceSize instBytes = sizeof(VkAccelerationStructureInstanceKHR) * ipc::MAX_INSTANCES;
    if (!CreateBuffer(instBytes,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m_instBuf)) {
        LogLine("[ACCEL][ERR] Failed to create TLAS instance buffer");
        return false;
    }

    LogLine("[ACCEL] BLAS/TLAS manager initialized");
    return true;
}

bool VkAccel::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags props, VkBuf& out)
{
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_vk.device, &bci, nullptr, &out.buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_vk.device, out.buf, &req);

    VkMemoryAllocateFlagsInfo flagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flagsInfo.flags = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
        ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = flagsInfo.flags ? &flagsInfo : nullptr;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = g_vk.FindMemoryType(req.memoryTypeBits, props);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(g_vk.device, out.buf, nullptr);
        out.buf = VK_NULL_HANDLE;
        return false;
    }

    if (vkAllocateMemory(g_vk.device, &mai, nullptr, &out.mem) != VK_SUCCESS) {
        vkDestroyBuffer(g_vk.device, out.buf, nullptr);
        out.buf = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(g_vk.device, out.buf, out.mem, 0);
    out.size = size;

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo bdai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        bdai.buffer = out.buf;
        out.addr = vkGetBufferDeviceAddress(g_vk.device, &bdai);
    }
    return true;
}

void VkAccel::DestroyBuffer(VkBuf& b) {
    if (b.buf) vkDestroyBuffer(g_vk.device, b.buf, nullptr);
    if (b.mem) vkFreeMemory(g_vk.device, b.mem, nullptr);
    b = {};
}

bool VkAccel::EnsureScratch(VkDeviceSize needed) {
    if (m_scratch.size >= needed) return true;
    DestroyBuffer(m_scratch);
    VkDeviceSize allocSize = needed < (4 * 1024 * 1024) ? (4 * 1024 * 1024) : needed;
    return CreateBuffer(allocSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_scratch);
}

void VkAccel::SubmitOneShot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(g_vk.computeQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.computeQueue);
}

bool VkAccel::BuildBLAS(uint64_t meshId, const float* verts, uint32_t vCount,
    const uint32_t* indices, uint32_t iCount)
{
    if (m_blasCache.count(meshId)) return true;
    if (vCount == 0 || iCount < 3) return false;

    BlasEntry entry{};
    VkDeviceSize vBytes = static_cast<VkDeviceSize>(vCount) * 3 * sizeof(float);
    VkDeviceSize iBytes = static_cast<VkDeviceSize>(iCount) * sizeof(uint32_t);

    auto geomUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    auto hostProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if (!CreateBuffer(vBytes, geomUsage, hostProps, entry.vb) ||
        !CreateBuffer(iBytes, geomUsage, hostProps, entry.ib)) {
        DestroyBuffer(entry.vb);
        DestroyBuffer(entry.ib);
        return false;
    }

    void* ptr = nullptr;
    vkMapMemory(g_vk.device, entry.vb.mem, 0, vBytes, 0, &ptr);
    memcpy(ptr, verts, vBytes);
    vkUnmapMemory(g_vk.device, entry.vb.mem);

    vkMapMemory(g_vk.device, entry.ib.mem, 0, iBytes, 0, &ptr);
    memcpy(ptr, indices, iBytes);
    vkUnmapMemory(g_vk.device, entry.ib.mem);

    uint32_t primCount = iCount / 3;
    VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = entry.vb.addr;
    geom.geometry.triangles.vertexStride = 3 * sizeof(float);
    geom.geometry.triangles.maxVertex = vCount - 1;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geom.geometry.triangles.indexData.deviceAddress = entry.ib.addr;

    VkAccelerationStructureBuildGeometryInfoKHR bgi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1;
    bgi.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    fpGetBuildSizes(g_vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, &primCount, &sizes);

    if (!CreateBuffer(sizes.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, entry.asBuf) ||
        !EnsureScratch(sizes.buildScratchSize)) {
        DestroyBuffer(entry.asBuf);
        DestroyBuffer(entry.vb);
        DestroyBuffer(entry.ib);
        return false;
    }

    VkAccelerationStructureCreateInfoKHR asci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    asci.buffer = entry.asBuf.buf;
    asci.size = sizes.accelerationStructureSize;
    asci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (fpCreateAS(g_vk.device, &asci, nullptr, &entry.as) != VK_SUCCESS) {
        DestroyBuffer(entry.asBuf);
        DestroyBuffer(entry.vb);
        DestroyBuffer(entry.ib);
        return false;
    }

    bgi.dstAccelerationStructure = entry.as;
    bgi.scratchData.deviceAddress = m_scratch.addr;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

    vkResetCommandBuffer(m_cmdBuf, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmdBuf, &bi);
    fpCmdBuildAS(m_cmdBuf, 1, &bgi, &pRange);
    SubmitOneShot(m_cmdBuf);

    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    addrInfo.accelerationStructure = entry.as;
    entry.asAddr = fpGetASAddr(g_vk.device, &addrInfo);
    DestroyBuffer(entry.vb);
    DestroyBuffer(entry.ib);

    m_blasCache[meshId] = entry;
    return true;
}

bool VkAccel::BuildTLAS(const ipc::InstanceDesc* instances, uint32_t count) {
    if (count == 0) {
        m_tlasInstCount = 0;
        return false;
    }

    std::vector<VkAccelerationStructureInstanceKHR> vkInsts;
    vkInsts.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        auto it = m_blasCache.find(instances[i].meshId);
        if (it == m_blasCache.end()) continue;

        VkAccelerationStructureInstanceKHR inst{};
        memcpy(inst.transform.matrix, instances[i].world4x3, sizeof(float) * 12);
        inst.instanceCustomIndex = i;
        inst.mask = 0xFF;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = it->second.asAddr;
        vkInsts.push_back(inst);
    }

    m_tlasInstCount = static_cast<uint32_t>(vkInsts.size());
    if (m_tlasInstCount == 0) return false;

    void* ptr = nullptr;
    vkMapMemory(g_vk.device, m_instBuf.mem, 0, sizeof(VkAccelerationStructureInstanceKHR) * m_tlasInstCount, 0, &ptr);
    memcpy(ptr, vkInsts.data(), sizeof(VkAccelerationStructureInstanceKHR) * m_tlasInstCount);
    vkUnmapMemory(g_vk.device, m_instBuf.mem);

    VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.arrayOfPointers = VK_FALSE;
    geom.geometry.instances.data.deviceAddress = m_instBuf.addr;

    VkAccelerationStructureBuildGeometryInfoKHR bgi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1;
    bgi.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    fpGetBuildSizes(g_vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, &m_tlasInstCount, &sizes);

    if (m_tlasBuf.size < sizes.accelerationStructureSize) {
        if (m_tlas) { fpDestroyAS(g_vk.device, m_tlas, nullptr); m_tlas = VK_NULL_HANDLE; }
        DestroyBuffer(m_tlasBuf);
        if (!CreateBuffer(sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_tlasBuf)) {
            return false;
        }
        VkAccelerationStructureCreateInfoKHR asci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        asci.buffer = m_tlasBuf.buf;
        asci.size = sizes.accelerationStructureSize;
        asci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (fpCreateAS(g_vk.device, &asci, nullptr, &m_tlas) != VK_SUCCESS) return false;
    }

    if (!EnsureScratch(sizes.buildScratchSize)) return false;

    bgi.dstAccelerationStructure = m_tlas;
    bgi.scratchData.deviceAddress = m_scratch.addr;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = m_tlasInstCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

    vkResetCommandBuffer(m_cmdBuf, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmdBuf, &bi);
    fpCmdBuildAS(m_cmdBuf, 1, &bgi, &pRange);
    SubmitOneShot(m_cmdBuf);
    return true;
}

void VkAccel::Shutdown() {
    if (!g_vk.device) return;
    vkDeviceWaitIdle(g_vk.device);
    if (m_tlas && fpDestroyAS) { fpDestroyAS(g_vk.device, m_tlas, nullptr); m_tlas = VK_NULL_HANDLE; }
    DestroyBuffer(m_tlasBuf);
    DestroyBuffer(m_instBuf);
    DestroyBuffer(m_scratch);
    for (auto& [id, e] : m_blasCache) {
        if (e.as && fpDestroyAS) fpDestroyAS(g_vk.device, e.as, nullptr);
        DestroyBuffer(e.asBuf);
        DestroyBuffer(e.vb);
        DestroyBuffer(e.ib);
    }
    m_blasCache.clear();
    if (m_cmdBuf) { vkFreeCommandBuffers(g_vk.device, g_vk.cmdPool, 1, &m_cmdBuf); m_cmdBuf = VK_NULL_HANDLE; }
}