#include <windows.h>
#include <d3d9.h>
#include "../core/log.h"
#include "../core/hooks.h"
#include "ipc_client.h"
#include "d3d9_hooks.h"

static HMODULE g_selfMod = nullptr;
static HMODULE g_sysD3D9 = nullptr;

HMODULE GetSelfModule() {
    return g_selfMod;
}

static bool LoadSystemD3D9() {
    if (g_sysD3D9) return true;
    char sysPath[MAX_PATH]{};
    GetSystemDirectoryA(sysPath, MAX_PATH);
    strcat_s(sysPath, "\\d3d9.dll");
    g_sysD3D9 = LoadLibraryA(sysPath);
    return g_sysD3D9 != nullptr;
}

extern "C" {

    IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT SDKVersion) {
        if (!LoadSystemD3D9()) return nullptr;
        using FnEx = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
        auto fnEx = reinterpret_cast<FnEx>(GetProcAddress(g_sysD3D9, "Direct3DCreate9Ex"));
        IDirect3D9Ex* d3dEx = nullptr;
        if (fnEx && SUCCEEDED(fnEx(SDKVersion, &d3dEx)) && d3dEx) {
            LogLine("[INFO] Direct3DCreate9 -> Direct3DCreate9Ex");
            InstallD3D9CreateHooks(d3dEx);
            return d3dEx;
        }
        return nullptr;
    }

    HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
        if (!LoadSystemD3D9()) return E_FAIL;
        using FnEx = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
        auto fnEx = reinterpret_cast<FnEx>(GetProcAddress(g_sysD3D9, "Direct3DCreate9Ex"));
        HRESULT hr = fnEx ? fnEx(SDKVersion, ppD3D) : E_FAIL;
        if (SUCCEEDED(hr) && ppD3D && *ppD3D) {
            LogLine("[INFO] Direct3DCreate9Ex called directly");
            InstallD3D9CreateHooks(*ppD3D);
        }
        return hr;
    }

    int   WINAPI Proxy_D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR) { return 0; }
    int   WINAPI Proxy_D3DPERF_EndEvent() { return 0; }
    void  WINAPI Proxy_D3DPERF_SetMarker(D3DCOLOR, LPCWSTR) {}
    void  WINAPI Proxy_D3DPERF_SetRegion(D3DCOLOR, LPCWSTR) {}
    BOOL  WINAPI Proxy_D3DPERF_QueryRepeatFrame() { return FALSE; }
    void  WINAPI Proxy_D3DPERF_SetOptions(DWORD) {}
    DWORD WINAPI Proxy_D3DPERF_GetStatus() { return 0; }

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfMod = hMod;
        DisableThreadLibraryCalls(hMod);
        InitLog(hMod);
        InitHooks();
        LogLine("[INFO] DllMain ATTACH");
    }
    if (reason == DLL_PROCESS_DETACH) {
        g_ipc.Shutdown();
        ShutdownHooks();
        if (g_sysD3D9) FreeLibrary(g_sysD3D9);
    }
    return TRUE;
}