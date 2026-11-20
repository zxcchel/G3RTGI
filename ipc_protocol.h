#pragma once
#include <cstdint>

namespace ipc {

    constexpr const wchar_t* SHM_NAME = L"Local\\G3_RTGI_SHM_V1";
    constexpr const wchar_t* EVT_FRAME_READY = L"Local\\G3_RTGI_FRAME_READY";
    constexpr const wchar_t* EVT_FRAME_DONE = L"Local\\G3_RTGI_FRAME_DONE";

    constexpr uint32_t MAX_INSTANCES = 8192;

    enum class Command : uint32_t {
        None = 0,
        Render = 1,
        Shutdown = 2,
    };

    struct alignas(16) InstanceDesc {
        uint64_t meshId;
        uint32_t flags;
        uint32_t pad;
        float    world4x3[12];
    };

    struct alignas(16) FrameHeader {
        uint32_t command;          // ipc::Command
        uint32_t frameIndex;
        uint32_t width;
        uint32_t height;

        // D3D9Ex shared KMT handles (передаём как uint64_t для 32->64 совместимости)
        uint64_t sharedColorHandle;
        uint64_t sharedOutHandle;

        // Камера и солнце
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
        FrameHeader  header;
        InstanceDesc instances[MAX_INSTANCES];
    };

} // namespace ipc