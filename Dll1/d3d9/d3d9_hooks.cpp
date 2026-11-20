#include "d3d9_hooks.h"
#include "ctab_parser.h"
#include "ipc_client.h"
#include "../core/log.h"
#include "../core/hooks.h"
#include "scene_tracker.h"
#include "geom_tracker.h"
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <string>

HMODULE GetSelfModule();

static std::atomic<bool> g_capturing{ false };
static bool g_keyWasDown = false;

static std::mutex g_shaderMtx;
static std::unordered_map<IDirect3DVertexShader9*, ShaderInfo> g_vsMap;
static std::unordered_map<IDirect3DPixelShader9*, ShaderInfo> g_psMap;
static IDirect3DVertexShader9* g_curVS = nullptr;
static IDirect3DPixelShader9* g_curPS = nullptr;
static uint32_t g_dipCount = 0;
static uint32_t g_backbufW = 1280;
static uint32_t g_backbufH = 768;
static IDirect3DTexture9* g_sharedOutTex = nullptr;
static IDirect3DTexture9* g_whiteTex = nullptr;
static IDirect3DPixelShader9* g_unlitPS[8] = {};
static HANDLE             g_sharedOutHandle = nullptr;
static IDirect3DStateBlock9* g_savedState = nullptr;
static bool                  g_fullscreenGI = false;
static int g_texSlotIdx = 1; // 0 была зелёная нормаль, начинаем сразу с 1
static void DrawAdditiveFullscreenGI(IDirect3DDevice9* dev, IDirect3DTexture9* tex, UINT w, UINT h) {
    if (!g_savedState) {
        dev->CreateStateBlock(D3DSBT_ALL, &g_savedState);
    }
    if (g_savedState) g_savedState->Capture();

    struct QuadVert { float x, y, z, rhw, u, v; };
    float fw = static_cast<float>(w), fh = static_cast<float>(h);
    QuadVert q[4] = {
        { -0.5f,      -0.5f,      0.f, 1.f, 0.f, 0.f },
        { fw - 0.5f,  -0.5f,      0.f, 1.f, 1.f, 0.f },
        { -0.5f,      fh - 0.5f,  0.f, 1.f, 0.f, 1.f },
        { fw - 0.5f,  fh - 0.5f,  0.f, 1.f, 1.f, 1.f },
    };

    if (SUCCEEDED(dev->BeginScene())) {
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetTexture(0, tex);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVert));
        dev->EndScene();
    }

    if (g_savedState) g_savedState->Apply();
}

using FnCreateDevice = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using FnCreateDeviceEx = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using FnReset = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using FnPresent = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using FnCreateTexture = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using FnCreateVB = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using FnCreateIB = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
using FnDrawIndexedPrim = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using FnCreateVS = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using FnSetVS = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using FnCreatePS = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using FnSetPS = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using FnSetConstF = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using FnSetViewport = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using FnSetRenderTarget = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);

static FnSetRenderTarget o_SetRenderTarget = nullptr;
static FnSetViewport     o_SetViewport = nullptr;
static FnSetConstF       o_SetVSConstF = nullptr;
static FnSetConstF       o_SetPSConstF = nullptr;
static FnCreateDevice    o_CreateDevice = nullptr;
static FnCreateDeviceEx  o_CreateDeviceEx = nullptr;
static FnReset           o_Reset = nullptr;
static FnPresent         o_Present = nullptr;
static FnCreateTexture   o_CreateTexture = nullptr;
static FnCreateVB        o_CreateVB = nullptr;
static FnCreateIB        o_CreateIB = nullptr;
static FnDrawIndexedPrim o_DrawIndexedPrim = nullptr;
static FnCreateVS        o_CreateVS = nullptr;
static FnSetVS           o_SetVS = nullptr;
static FnCreatePS        o_CreatePS = nullptr;
static FnSetPS           o_SetPS = nullptr;

static void ReleaseSharedOut() {
    if (g_savedState) { g_savedState->Release(); g_savedState = nullptr; }
    if (g_sharedOutTex) { g_sharedOutTex->Release(); g_sharedOutTex = nullptr; }
    if (g_whiteTex) { g_whiteTex->Release(); g_whiteTex = nullptr; }
    for (int i = 0; i < 8; ++i) {
        if (g_unlitPS[i]) { g_unlitPS[i]->Release(); g_unlitPS[i] = nullptr; }
    }
    g_sharedOutHandle = nullptr;
}

static HRESULT STDMETHODCALLTYPE hk_SetRenderTarget(IDirect3DDevice9* dev, DWORD rtIdx, IDirect3DSurface9* pRT) {
    if (rtIdx == 0 && pRT) {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(pRT->GetDesc(&desc))) {
            g_scene.SetMainPass(desc.Width == g_backbufW && desc.Height == g_backbufH);
        }
    }
    return o_SetRenderTarget(dev, rtIdx, pRT);
}

static void EnsureSharedOut(IDirect3DDevice9* dev) {
    if (g_sharedOutTex || g_backbufW == 0 || g_backbufH == 0) return;
    HANDLE h = nullptr;
    HRESULT hr = o_CreateTexture(dev, g_backbufW, g_backbufH, 1,
        D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &g_sharedOutTex, &h);
    if (SUCCEEDED(hr) && g_sharedOutTex && h) {
        g_sharedOutHandle = h;
        LogLine("[INFO] Created D3D9Ex shared output texture");
    }
    else {
        LogLine("[ERR] Failed to create shared output texture");
    }
}
static void EnsureWhiteTex(IDirect3DDevice9* dev) {
    if (g_whiteTex) return;
    if (SUCCEEDED(o_CreateTexture(dev, 1, 1, 1, D3DUSAGE_DYNAMIC,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_whiteTex, nullptr)) && g_whiteTex)
    {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(g_whiteTex->LockRect(0, &lr, nullptr, 0))) {
            *static_cast<uint32_t*>(lr.pBits) = 0xFFFFFFFFu;
            g_whiteTex->UnlockRect(0);
        }
    }
}
static void EnsureUnlitPS(IDirect3DDevice9* dev) {
    if (g_unlitPS[0]) return;
    for (int i = 0; i < 8; ++i) {
        char src[256];
        snprintf(src, sizeof(src),
            "sampler2D s : register(s%d); "
            "float4 main(float2 uv : TEXCOORD0) : COLOR0 { return float4(tex2D(s, uv).rgb, 1.0); }", i);
        ID3DBlob* blob = nullptr;
        if (SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
            "main", "ps_3_0", 0, 0, &blob, nullptr)) && blob)
        {
            o_CreatePS(dev, static_cast<const DWORD*>(blob->GetBufferPointer()), &g_unlitPS[i]);
            blob->Release();
        }
    }
}
static HRESULT STDMETHODCALLTYPE hk_SetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* pVP) {
    if (pVP) {
        g_scene.SetMainPass(pVP->Width == g_backbufW && pVP->Height == g_backbufH);
    }
    return o_SetViewport(dev, pVP);
}

static HRESULT STDMETHODCALLTYPE hk_SetVSConstF(IDirect3DDevice9* dev, UINT startReg, const float* pData, UINT count) {
    g_scene.OnSetVSConstF(startReg, pData, count);
    float patched[256 * 4];
    if (g_scene.FilterVSConstF(startReg, pData, count, patched)) {
        return o_SetVSConstF(dev, startReg, patched, count);
    }
    return o_SetVSConstF(dev, startReg, pData, count);
}

static HRESULT STDMETHODCALLTYPE hk_SetPSConstF(IDirect3DDevice9* dev, UINT startReg, const float* pData, UINT count) {
    float patched[256 * 4];
    if (g_scene.FilterPSConstF(startReg, pData, count, patched)) {
        return o_SetPSConstF(dev, startReg, patched, count);
    }
    return o_SetPSConstF(dev, startReg, pData, count);
}

static HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    ReleaseSharedOut();
    g_geom.ClearCache();
    if (pp && pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0) {
        g_backbufW = pp->BackBufferWidth;
        g_backbufH = pp->BackBufferHeight;
    }
    auto* devEx = static_cast<IDirect3DDevice9Ex*>(dev);
    D3DDISPLAYMODEEX mode{};
    D3DDISPLAYMODEEX* pMode = nullptr;
    if (pp && !pp->Windowed) {
        mode.Size = sizeof(D3DDISPLAYMODEEX);
        mode.Width = pp->BackBufferWidth;
        mode.Height = pp->BackBufferHeight;
        mode.RefreshRate = pp->FullScreen_RefreshRateInHz;
        mode.Format = pp->BackBufferFormat;
        mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
        pMode = &mode;
    }
    return devEx->ResetEx(pp, pMode);
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture(
    IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage,
    D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** ppTex, HANDLE* pShared)
{
    if (pool == D3DPOOL_MANAGED) {
        pool = D3DPOOL_DEFAULT;
        if (!(usage & D3DUSAGE_RENDERTARGET) && !(usage & D3DUSAGE_DEPTHSTENCIL)) {
            usage |= D3DUSAGE_DYNAMIC;
            usage &= ~D3DUSAGE_AUTOGENMIPMAP;
        }
    }
    return o_CreateTexture(dev, w, h, levels, usage, fmt, pool, ppTex, pShared);
}

static HRESULT STDMETHODCALLTYPE hk_CreateVB(
    IDirect3DDevice9* dev, UINT len, DWORD usage, DWORD fvf,
    D3DPOOL pool, IDirect3DVertexBuffer9** ppVB, HANDLE* pShared)
{
    if (pool == D3DPOOL_MANAGED) {
        pool = D3DPOOL_DEFAULT;
        usage |= D3DUSAGE_DYNAMIC;
        usage &= ~D3DUSAGE_WRITEONLY;
    }
    return o_CreateVB(dev, len, usage, fvf, pool, ppVB, pShared);
}

static HRESULT STDMETHODCALLTYPE hk_CreateIB(
    IDirect3DDevice9* dev, UINT len, DWORD usage, D3DFORMAT fmt,
    D3DPOOL pool, IDirect3DIndexBuffer9** ppIB, HANDLE* pShared)
{
    if (pool == D3DPOOL_MANAGED) {
        pool = D3DPOOL_DEFAULT;
        usage |= D3DUSAGE_DYNAMIC;
        usage &= ~D3DUSAGE_WRITEONLY;
    }
    return o_CreateIB(dev, len, usage, fmt, pool, ppIB, pShared);
}

static HRESULT STDMETHODCALLTYPE hk_CreateVS(IDirect3DDevice9* dev, const DWORD* pFunc, IDirect3DVertexShader9** ppVS) {
    HRESULT hr = o_CreateVS(dev, pFunc, ppVS);
    if (SUCCEEDED(hr) && ppVS && *ppVS) {
        ShaderInfo info = ParseShaderCTAB(reinterpret_cast<const uint32_t*>(pFunc), false);
        std::lock_guard<std::mutex> lk(g_shaderMtx);
        g_scene.RegisterVS(*ppVS, info);
        g_vsMap[*ppVS] = std::move(info);
    }
    return hr;
}
static int FindDiffuseSlotFromDisasm(const DWORD* pFunc, const ShaderInfo& info) {
    int candidates[8]{};
    int candCount = 0;
    for (const auto& c : info.constants) {
        if (c.name.rfind("Sampler2D", 0) == 0 && c.regIndex < 8) {
            int r = static_cast<int>(c.regIndex);
            bool dup = false;
            for (int i = 0; i < candCount; ++i) if (candidates[i] == r) dup = true;
            if (!dup && candCount < 8) candidates[candCount++] = r;
        }
    }
    if (candCount == 0) return -1;
    if (candCount == 1) return candidates[0];

    // Ищем конец байткода D3D9 (токен 0x0000FFFF)
    size_t dwordCount = 0;
    while (dwordCount < 16384 && pFunc[dwordCount] != 0x0000FFFFu) ++dwordCount;
    if (dwordCount == 0 || dwordCount >= 16384) return candidates[0];

    ID3DBlob* disasm = nullptr;
    if (FAILED(D3DDisassemble(pFunc, (dwordCount + 1) * sizeof(DWORD), 0, nullptr, &disasm)) || !disasm) {
        return candidates[0];
    }

    std::string text(static_cast<const char*>(disasm->GetBufferPointer()), disasm->GetBufferSize());
    disasm->Release();

    bool isNormal[8]{};
    size_t pos = 0;
    while ((pos = text.find("texld", pos)) != std::string::npos) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);

        // Парсим "texld[_pp] rX, ..., sY"
        size_t rPos = line.find(" r");
        size_t sPos = line.rfind(", s");
        if (rPos != std::string::npos && sPos != std::string::npos) {
            size_t rEnd = line.find_first_of(",. ", rPos + 1);
            std::string regName = line.substr(rPos + 1, rEnd - (rPos + 1)); // например "r0"
            int sIdx = atoi(line.c_str() + sPos + 3);

            if (sIdx >= 0 && sIdx < 8) {
                // Смотрим ближайшие 500 символов после texld на DXT5nm-распаковку (.wy / .ag)
                std::string window = text.substr(eol, 500);
                if (window.find(regName + ".wy") != std::string::npos ||
                    window.find(regName + ".yw") != std::string::npos ||
                    window.find(regName + ".ag") != std::string::npos ||
                    window.find(regName + ".ga") != std::string::npos)
                {
                    isNormal[sIdx] = true;
                }
            }
        }
        pos = eol;
    }

    // Берём первый Sampler2D, который НЕ является DXT5nm-нормалью
    for (int i = 0; i < candCount; ++i) {
        if (!isNormal[candidates[i]]) return candidates[i];
    }
    return candidates[0];
}
static HRESULT STDMETHODCALLTYPE hk_CreatePS(IDirect3DDevice9* dev, const DWORD* pFunc, IDirect3DPixelShader9** ppPS) {
    HRESULT hr = o_CreatePS(dev, pFunc, ppPS);
    if (SUCCEEDED(hr) && ppPS && *ppPS) {
        ShaderInfo info = ParseShaderCTAB(reinterpret_cast<const uint32_t*>(pFunc), true);
        int diffSlot = FindDiffuseSlotFromDisasm(pFunc, info);
        std::lock_guard<std::mutex> lk(g_shaderMtx);
        g_scene.RegisterPS(*ppPS, info, diffSlot);
        g_psMap[*ppPS] = std::move(info);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetVS(IDirect3DDevice9* dev, IDirect3DVertexShader9* pVS) {
    g_scene.OnSetVS(pVS);
    g_curVS = pVS;
    return o_SetVS(dev, pVS);
}

static HRESULT STDMETHODCALLTYPE hk_SetPS(IDirect3DDevice9* dev, IDirect3DPixelShader9* pPS) {
    g_scene.OnSetPS(pPS);
    g_curPS = pPS;
    return o_SetPS(dev, pPS);
}

static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrim(
    IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT baseV, UINT minV,
    UINT numV, UINT startIdx, UINT primCount)
{
    if (g_capturing.load(std::memory_order_relaxed)) {
        g_dipCount++;
        uint64_t vsHash = 0, psHash = 0;
        {
            std::lock_guard<std::mutex> lk(g_shaderMtx);
            if (auto it = g_vsMap.find(g_curVS); it != g_vsMap.end()) vsHash = it->second.hash;
            if (auto it = g_psMap.find(g_curPS); it != g_psMap.end()) psHash = it->second.hash;
        }
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT offset = 0, stride = 0;
        dev->GetStreamSource(0, &vb, &offset, &stride);
        if (vb) vb->Release();

        char buf[256];
        snprintf(buf, sizeof(buf), "DIP #%u | type=%u numV=%u prim=%u stride=%u | VS=0x%016llX PS=0x%016llX",
            g_dipCount, (unsigned)type, numV, primCount, stride,
            (unsigned long long)vsHash, (unsigned long long)psHash);
        LogLine(buf);
    }
    g_geom.OnDrawIndexedPrim(dev, type, baseV, minV, numV, startIdx, primCount);

    if (g_scene.IsAmbientDisabled() && g_scene.IsMainPass() && g_scene.IsWorldLitPS()) {
        DWORD zWrite = TRUE, alphaBlend = FALSE, alphaTest = FALSE;
        dev->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
        dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
        dev->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
        int slot = g_scene.GetDiffuseSlot();
        if (zWrite && !alphaBlend && !alphaTest && slot >= 0 && slot < 8) {
            EnsureUnlitPS(dev);
            if (g_unlitPS[slot]) {
                IDirect3DPixelShader9* prevPS = g_curPS;
                o_SetPS(dev, g_unlitPS[slot]);
                HRESULT hr = o_DrawIndexedPrim(dev, type, baseV, minV, numV, startIdx, primCount);
                o_SetPS(dev, prevPS);
                return hr;
            }
        }
    }
    return o_DrawIndexedPrim(dev, type, baseV, minV, numV, startIdx, primCount);
}

static HRESULT STDMETHODCALLTYPE hk_Present(
    IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    static bool firstPresent = true;
    if (firstPresent) {
        firstPresent = false;
        LogLine("[INFO] First Present reached!");
    }

    if (g_ipc.IsReady()) {
        EnsureSharedOut(dev);
        auto* shm = g_ipc.Data();
        shm->header.command = static_cast<uint32_t>(ipc::Command::Render);
        shm->header.frameIndex++;
        shm->header.width = g_backbufW;
        shm->header.height = g_backbufH;
        shm->header.sharedOutHandle = reinterpret_cast<uintptr_t>(g_sharedOutHandle);
        g_geom.FillInstances(shm);
        g_scene.FillFrameHeader(shm->header);

        static bool f9WasDown = false;
        bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9Down && !f9WasDown) {
            g_scene.ToggleDisableAmbient();
            Beep(g_scene.IsAmbientDisabled() ? 800 : 400, 80);
        }
        f9WasDown = f9Down;
        static bool f10WasDown = false;
        bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10Down && !f10WasDown) {
            g_fullscreenGI = !g_fullscreenGI;
            Beep(g_fullscreenGI ? 1000 : 500, 80);
        }
        f10WasDown = f10Down;

        if (g_ipc.SubmitFrameAndWait(2000) && g_sharedOutTex) {
            if (g_fullscreenGI) {
                DrawAdditiveFullscreenGI(dev, g_sharedOutTex, g_backbufW, g_backbufH);
            }
            else {
                IDirect3DSurface9* srcSurf = nullptr;
                IDirect3DSurface9* backBuf = nullptr;
                if (SUCCEEDED(g_sharedOutTex->GetSurfaceLevel(0, &srcSurf)) &&
                    SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuf))) {
                    RECT dstRect{ 20, 20, 276, 148 };
                    dev->StretchRect(srcSurf, nullptr, backBuf, &dstRect, D3DTEXF_NONE);
                }
                if (srcSurf) srcSurf->Release();
                if (backBuf) backBuf->Release();
            }
        }
        g_geom.BeginFrame();
    }

    if (g_capturing.load(std::memory_order_relaxed)) {
        g_capturing.store(false, std::memory_order_relaxed);
        LogLine("=== END FRAME DUMP (total DIP: " + std::to_string(g_dipCount) + ") ===");
        Beep(1000, 150);
    }

    bool keyDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (keyDown && !g_keyWasDown) {
        g_dipCount = 0;
        LogLine("=== BEGIN FRAME DUMP (INSERT) ===");
        g_capturing.store(true, std::memory_order_relaxed);
        Beep(600, 100);
    }
    g_keyWasDown = keyDown;

    return o_Present(dev, src, dst, wnd, dirty);
}

static void InstallDeviceHooks(IDirect3DDevice9Ex* devEx) {
    LogLine("[INFO] Installing hooks on IDirect3DDevice9Ex");
    HookVtable(devEx, 16, hk_Reset, &o_Reset);
    HookVtable(devEx, 17, hk_Present, &o_Present);
    HookVtable(devEx, 23, hk_CreateTexture, &o_CreateTexture);
    HookVtable(devEx, 26, hk_CreateVB, &o_CreateVB);
    HookVtable(devEx, 27, hk_CreateIB, &o_CreateIB);
    HookVtable(devEx, 82, hk_DrawIndexedPrim, &o_DrawIndexedPrim);
    HookVtable(devEx, 91, hk_CreateVS, &o_CreateVS);
    HookVtable(devEx, 92, hk_SetVS, &o_SetVS);
    HookVtable(devEx, 106, hk_CreatePS, &o_CreatePS);
    HookVtable(devEx, 107, hk_SetPS, &o_SetPS);
    HookVtable(devEx, 94, hk_SetVSConstF, &o_SetVSConstF);
    HookVtable(devEx, 109, hk_SetPSConstF, &o_SetPSConstF);
    HookVtable(devEx, 47, hk_SetViewport, &o_SetViewport);
    HookVtable(devEx, 37, hk_SetRenderTarget, &o_SetRenderTarget);

    g_ipc.Init(GetSelfModule());
}

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(
    IDirect3D9* d3d, UINT adapter, D3DDEVTYPE devType, HWND focusWnd,
    DWORD behaviorFlags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** ppDev)
{
    if (pp && pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0) {
        g_backbufW = pp->BackBufferWidth;
        g_backbufH = pp->BackBufferHeight;
    }
    auto* d3dEx = static_cast<IDirect3D9Ex*>(d3d);
    D3DDISPLAYMODEEX mode{};
    D3DDISPLAYMODEEX* pMode = nullptr;
    if (pp && !pp->Windowed) {
        mode.Size = sizeof(D3DDISPLAYMODEEX);
        mode.Width = pp->BackBufferWidth;
        mode.Height = pp->BackBufferHeight;
        mode.RefreshRate = pp->FullScreen_RefreshRateInHz;
        mode.Format = pp->BackBufferFormat;
        mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
        pMode = &mode;
    }

    IDirect3DDevice9Ex* devEx = nullptr;
    HRESULT hr = o_CreateDeviceEx
        ? o_CreateDeviceEx(d3dEx, adapter, devType, focusWnd, behaviorFlags, pp, pMode, &devEx)
        : d3dEx->CreateDeviceEx(adapter, devType, focusWnd, behaviorFlags, pp, pMode, &devEx);
    if (SUCCEEDED(hr) && devEx) {
        *ppDev = devEx;
        InstallDeviceHooks(devEx);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDeviceEx(
    IDirect3D9Ex* d3dEx, UINT adapter, D3DDEVTYPE devType, HWND focusWnd,
    DWORD behaviorFlags, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* pMode, IDirect3DDevice9Ex** ppDev)
{
    if (pp && pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0) {
        g_backbufW = pp->BackBufferWidth;
        g_backbufH = pp->BackBufferHeight;
    }
    HRESULT hr = o_CreateDeviceEx(d3dEx, adapter, devType, focusWnd, behaviorFlags, pp, pMode, ppDev);
    if (SUCCEEDED(hr) && ppDev && *ppDev) {
        LogLine("[INFO] CreateDeviceEx succeeded");
        InstallDeviceHooks(*ppDev);
    }
    return hr;
}

void InstallD3D9CreateHooks(IDirect3D9Ex* d3dEx) {
    HookVtable(d3dEx, 16, hk_CreateDevice, &o_CreateDevice);
    HookVtable(d3dEx, 20, hk_CreateDeviceEx, &o_CreateDeviceEx);
}