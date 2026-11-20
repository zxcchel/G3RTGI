#include <windows.h>
#include <fstream>
#include <string>
#include "../shared/ipc_protocol.h"
#include "vk/vk_context.h"
#include "vk/vk_interop.h"
#include "vk/vk_accel.h"
#include "vk/vk_rt_pipeline.h"

static std::ofstream g_wlog;
void LogLine(const std::string& s) {
    if (!g_wlog.is_open()) g_wlog.open("g3_rtgi_worker.log", std::ios::out | std::ios::trunc);
    g_wlog << s << "\n";
    g_wlog.flush();
}

int main() {
    // Прибиваем воркер к свободным физическим ядрам (CPU5..CPU8), чтобы не мешать главному потоку Gothic 3
    SetProcessAffinityMask(GetCurrentProcess(), 0xF0);

    LogLine("[WORKER] 64-bit RTGI worker started");

    if (!g_vk.Init()) {
        LogLine("[WORKER][ERR] Vulkan 64-bit init failed");
        return 1;
    }

    if (!g_accel.Init()) {
        LogLine("[WORKER][ERR] VkAccel init failed");
        g_vk.Shutdown();
        return 1;
    }

    if (!g_rtPipe.Init("rt_test.spv")) {
        LogLine("[WORKER][WARN] VkRtPipeline init failed (rt_test.spv missing?), falling back to test clear");
    }

    HANDLE hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, ipc::SHM_NAME);
    HANDLE hReady = OpenEventW(SYNCHRONIZE, FALSE, ipc::EVT_FRAME_READY);
    HANDLE hDone = OpenEventW(EVENT_MODIFY_STATE, FALSE, ipc::EVT_FRAME_DONE);
    if (!hMap || !hReady || !hDone) {
        LogLine("[WORKER][ERR] Failed to open IPC objects");
        g_rtPipe.Shutdown();
        g_accel.Shutdown();
        g_vk.Shutdown();
        return 2;
    }

    auto* shm = static_cast<ipc::SharedMemoryLayout*>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::SharedMemoryLayout)));

    LogLine("[WORKER] IPC connected, entering frame loop...");
    VkInteropSurface interop;
    bool firstFrame = true;
    uint32_t frameLogCounter = 0;

    while (true) {
        DWORD w = WaitForSingleObject(hReady, 5000);
        if (w == WAIT_TIMEOUT) continue;
        if (w != WAIT_OBJECT_0) break;

        auto cmd = static_cast<ipc::Command>(shm->header.command);
        if (cmd == ipc::Command::Shutdown) {
            LogLine("[WORKER] Shutdown command received");
            break;
        }

        if (cmd == ipc::Command::UploadMesh) {
            g_accel.BuildBLAS(shm->meshUpload.meshId,
                shm->meshVertices, shm->meshUpload.vertexCount,
                shm->meshIndices, shm->meshUpload.indexCount);
            SetEvent(hDone);
            continue;
        }

        if (cmd == ipc::Command::Render) {
            if (firstFrame) {
                firstFrame = false;
                LogLine("[WORKER] First frame received! " +
                    std::to_string(shm->header.width) + "x" + std::to_string(shm->header.height) +
                    " instances=" + std::to_string(shm->header.instanceCount));
            }

            g_accel.BuildTLAS(shm->instances, shm->header.instanceCount);

            if (shm->header.sharedOutHandle != 0) {
                if (interop.Import(shm->header.sharedOutHandle, shm->header.width, shm->header.height)) {
                    if (g_accel.GetTLAS() != VK_NULL_HANDLE && g_accel.GetTlasInstanceCount() > 0) {
                        g_rtPipe.Dispatch(g_accel.GetTLAS(), interop.GetImage(), interop.GetView(), shm->header);
                    }
                    else {
                        interop.RenderTestClear(shm->header.frameIndex);
                    }
                }
            }

            if (++frameLogCounter % 120 == 0) {
                LogLine("[STATS] blasCount=" + std::to_string(g_accel.GetBlasCount()) +
                    " tlasInstances=" + std::to_string(g_accel.GetTlasInstanceCount()) +
                    " frameInstances=" + std::to_string(shm->header.instanceCount) +
                    " cam=(" + std::to_string(shm->header.camPos[0]) + "," +
                    std::to_string(shm->header.camPos[1]) + "," +
                    std::to_string(shm->header.camPos[2]) + ")");
            }
        }

        SetEvent(hDone);
    }

    interop.Destroy();
    UnmapViewOfFile(shm);
    CloseHandle(hDone);
    CloseHandle(hReady);
    CloseHandle(hMap);
    g_rtPipe.Shutdown();
    g_accel.Shutdown();
    g_vk.Shutdown();
    return 0;
}