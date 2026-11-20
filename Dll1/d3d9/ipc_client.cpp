#include "ipc_client.h"
#include "../core/log.h"
#include <string>

IpcClient g_ipc;

bool IpcClient::Init(HMODULE selfMod) {
    if (m_shm) return true;

    m_hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(ipc::SharedMemoryLayout), ipc::SHM_NAME);
    if (!m_hMap) {
        LogLine("[IPC][ERR] CreateFileMappingW failed");
        return false;
    }

    m_shm = static_cast<ipc::SharedMemoryLayout*>(
        MapViewOfFile(m_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::SharedMemoryLayout)));
    if (!m_shm) {
        LogLine("[IPC][ERR] MapViewOfFile failed");
        Shutdown();
        return false;
    }
    ZeroMemory(m_shm, sizeof(ipc::SharedMemoryLayout));

    m_hFrameReady = CreateEventW(nullptr, FALSE, FALSE, ipc::EVT_FRAME_READY);
    m_hFrameDone = CreateEventW(nullptr, FALSE, FALSE, ipc::EVT_FRAME_DONE);

    // Ищем g3_rtgi_worker.exe рядом с d3d9.dll
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(selfMod, path, MAX_PATH);
    std::wstring workerPath(path);
    auto pos = workerPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) workerPath.resize(pos + 1);
    workerPath += L"g3_rtgi_worker.exe";

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(workerPath.c_str(), nullptr, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        LogLine("[IPC][ERR] Failed to launch g3_rtgi_worker.exe");
        Shutdown();
        return false;
    }
    m_hWorkerProc = pi.hProcess;
    CloseHandle(pi.hThread);

    LogLine("[IPC] Shared memory ready & 64-bit worker launched!");
    return true;
}

bool IpcClient::SubmitFrameAndWait(DWORD timeoutMs) {
    if (!m_shm) return false;
    SetEvent(m_hFrameReady);
    return WaitForSingleObject(m_hFrameDone, timeoutMs) == WAIT_OBJECT_0;
}

void IpcClient::Shutdown() {
    if (m_shm) {
        m_shm->header.command = static_cast<uint32_t>(ipc::Command::Shutdown);
        if (m_hFrameReady) SetEvent(m_hFrameReady);
        UnmapViewOfFile(m_shm);
        m_shm = nullptr;
    }
    if (m_hWorkerProc) { WaitForSingleObject(m_hWorkerProc, 500); CloseHandle(m_hWorkerProc); m_hWorkerProc = nullptr; }
    if (m_hFrameReady) { CloseHandle(m_hFrameReady); m_hFrameReady = nullptr; }
    if (m_hFrameDone) { CloseHandle(m_hFrameDone);  m_hFrameDone = nullptr; }
    if (m_hMap) { CloseHandle(m_hMap);        m_hMap = nullptr; }
}