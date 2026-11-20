#pragma once
#include <cstdint>

namespace ipc {

    constexpr const wchar_t* SHM_NAME = L"Local\\G3_RTGI_SHM_V1";
    constexpr const wchar_t* EVT_FRAME_READY = L"Local\\G3_RTGI_FRAME_READY";
    constexpr const wchar_t* EVT_FRAME_DONE = L"Local\\G3_RTGI_FRAME_DONE";

    constexpr uint32_t MAX_INSTANCES = 8192;
    constexpr uint32_t MAX_MESH_VERTS = 131072; // ~1.5 МБ (float3)
    constexpr uint32_t MAX_MESH_INDXS = 393216; // ~1.5 МБ (uint32_t)

    enum class Command : uint32_t {
        None = 0,
        Render = 1,
        Shutdown = 2,
        UploadMesh = 3,
    };

    struct alignas(16) InstanceDesc {
        uint64_t meshId;
        uint32_t flags;
        uint32_t pad;
        float    world4x3[12];
    };

    struct alignas(16) MeshUploadHeader {
        uint64_t meshId;
        uint32_t vertexCount;
        uint32_t indexCount;
    };

    struct alignas(16) FrameHeader {
        uint32_t command;          // ipc::Command
        uint32_t frameIndex;
        uint32_t width;
        uint32_t height;

        uint64_t sharedColorHandle;
        uint64_t sharedOutHandle;

        float    viewProj[16];
        float    invView[12];
        float    camPos[4];
        float    sunDir[4];
        float    sunColor[4];
        float    skyColor[4];

        uint32_t instanceCount;
        uint32_t pad[3];
    };

    struct SharedMemoryLayout {
        FrameHeader      header;
        InstanceDesc     instances[MAX_INSTANCES];

        MeshUploadHeader meshUpload;
        float            meshVertices[MAX_MESH_VERTS * 3]; // плотный xyz
        uint32_t         meshIndices[MAX_MESH_INDXS];      // приведённые к uint32
    };

} // namespace ipc