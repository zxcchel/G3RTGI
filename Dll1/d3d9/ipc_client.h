#pragma once
#include <windows.h>
#include "../../shared/ipc_protocol.h"

class IpcClient {
public:
    bool Init(HMODULE selfMod);
    void Shutdown();

    ipc::SharedMemoryLayout* Data() { return m_shm; }
    bool SubmitFrameAndWait(DWORD timeoutMs = 1000);
    bool IsReady() const { return m_shm != nullptr; }

private:
    HANDLE m_hMap = nullptr;
    HANDLE m_hFrameReady = nullptr;
    HANDLE m_hFrameDone = nullptr;
    HANDLE m_hWorkerProc = nullptr;
    ipc::SharedMemoryLayout* m_shm = nullptr;
};

extern IpcClient g_ipc;