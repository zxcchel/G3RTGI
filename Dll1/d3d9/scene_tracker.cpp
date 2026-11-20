#include "scene_tracker.h"
#include <unordered_map>
#include <mutex>
#include <cstring>
#include "../core/log.h"
#include <unordered_set>
#include <algorithm>

SceneTracker g_scene;

static std::mutex g_slotMtx;
static std::unordered_map<IDirect3DVertexShader9*, VsSlotMap> g_vsSlots;
static std::unordered_map<IDirect3DPixelShader9*, PsSlotMap> g_psSlots;

static bool CopyRegRange(UINT startReg, UINT count, const float* data, int16_t targetReg, UINT targetVecs, float* dst) {
    if (targetReg < 0) return false;
    UINT t = static_cast<UINT>(targetReg);
    if (t >= startReg && (t + targetVecs) <= (startReg + count)) {
        memcpy(dst, data + (t - startReg) * 4, targetVecs * 4 * sizeof(float));
        return true;
    }
    return false;
}

void SceneTracker::LatchMainCamera() {
    if (!m_camLatched) {
        memcpy(m_latchedViewProj, m_viewProj, sizeof(m_viewProj));
        m_camLatched = true;
    }
}

void SceneTracker::RegisterVS(IDirect3DVertexShader9* vs, const ShaderInfo& info) {
    VsSlotMap m{};
    for (const auto& c : info.constants) {
        if (c.name == "matViewProjection")      m.regViewProj = static_cast<int16_t>(c.regIndex);
        else if (c.name == "matWorld")          m.regWorld = static_cast<int16_t>(c.regIndex);
        else if (c.name == "matInvView")        m.regInvView = static_cast<int16_t>(c.regIndex);
        else if (c.name == "vecCamPos")         m.regCamPos = static_cast<int16_t>(c.regIndex);
        else if (c.name == "vecSunNormal")      m.regSunDir = static_cast<int16_t>(c.regIndex);
        else if (c.name == "colSun")            m.regSunCol = static_cast<int16_t>(c.regIndex);
        else if (c.name == "colSky")            m.regSkyCol = static_cast<int16_t>(c.regIndex);
        if (c.name.find("Bone") != std::string::npos ||
            c.name.find("bone") != std::string::npos ||
            c.name.find("Skin") != std::string::npos) {
            m.isSkinned = true;
        }
    }
    std::lock_guard<std::mutex> lk(g_slotMtx);
    g_vsSlots[vs] = m;
}

void SceneTracker::RegisterPS(IDirect3DPixelShader9* ps, const ShaderInfo& info, int diffuseSlot) {
    PsSlotMap m{};
    m.diffuseSlot = static_cast<int8_t>(diffuseSlot);
    for (const auto& c : info.constants) {
        if (c.name == "arrSHCofficients")      m.regSH = static_cast<int16_t>(c.regIndex);
        else if (c.name == "DirLight")         m.regDirLight = static_cast<int16_t>(c.regIndex);
        else if (c.name == "colRimLight")      m.regRimLight = static_cast<int16_t>(c.regIndex);
        else if (c.name == "vecShadowDensity") m.regShadowDensity = static_cast<int16_t>(c.regIndex);
    }
    std::lock_guard<std::mutex> lk(g_slotMtx);
    g_psSlots[ps] = m;
}

bool SceneTracker::FilterPSConstF(UINT startReg, const float* data, UINT vec4Count, float* outBuf) {
    if (!m_disableAmbient || !m_curPS || !data || vec4Count > 256) return false;

    bool modified = false;
    auto ensureCopy = [&]() {
        if (!modified) {
            memcpy(outBuf, data, vec4Count * 4 * sizeof(float));
            modified = true;
        }
        };

    auto zeroRegs = [&](int16_t baseReg, UINT count) {
        if (baseReg < 0) return;
        UINT b = static_cast<UINT>(baseReg);
        if (b + count <= startReg || b >= startReg + vec4Count) return;
        ensureCopy();
        for (UINT r = b; r < b + count; ++r) {
            if (r >= startReg && r < startReg + vec4Count) {
                float* dst = outBuf + (r - startReg) * 4;
                dst[0] = dst[1] = dst[2] = dst[3] = 0.0f;
            }
        }
        };

    // 1. Ровный белый эмбиент вместо сферических гармоник
    if (m_curPS->regSH >= 0) {
        UINT sh = static_cast<UINT>(m_curPS->regSH);
        if (sh + 4 > startReg && sh < startReg + vec4Count) {
            ensureCopy();
            for (UINT r = sh; r < sh + 4; ++r) {
                if (r >= startReg && r < startReg + vec4Count) {
                    float* dst = outBuf + (r - startReg) * 4;
                    if (r == sh) {
                        dst[0] = dst[1] = dst[2] = dst[3] = 1.0f;
                    }
                    else {
                        dst[0] = dst[1] = dst[2] = dst[3] = 0.0f;
                    }
                }
            }
        }
    }

    // 2. Убиваем родное солнце (DirLight), rim-light и плотность ванильных теней
    zeroRegs(m_curPS->regDirLight, 3);
    zeroRegs(m_curPS->regRimLight, 1);
    zeroRegs(m_curPS->regShadowDensity, 1);

    return modified;
}

void SceneTracker::OnSetVS(IDirect3DVertexShader9* vs) {
    std::lock_guard<std::mutex> lk(g_slotMtx);
    auto it = g_vsSlots.find(vs);
    m_curVS = (it != g_vsSlots.end()) ? &it->second : nullptr;
    m_hasWorld = (m_curVS && m_curVS->regWorld >= 0);
}

void SceneTracker::OnSetPS(IDirect3DPixelShader9* ps) {
    std::lock_guard<std::mutex> lk(g_slotMtx);
    auto it = g_psSlots.find(ps);
    m_curPS = (it != g_psSlots.end()) ? &it->second : nullptr;
}

void SceneTracker::OnSetVSConstF(UINT startReg, const float* data, UINT vec4Count) {
    if (!m_isMainPass || !m_curVS || !data) return;
    CopyRegRange(startReg, vec4Count, data, m_curVS->regWorld, 3, m_world);
    CopyRegRange(startReg, vec4Count, data, m_curVS->regViewProj, 4, m_viewProj);
    CopyRegRange(startReg, vec4Count, data, m_curVS->regInvView, 3, m_invView);
    CopyRegRange(startReg, vec4Count, data, m_curVS->regCamPos, 1, m_camPos);
    CopyRegRange(startReg, vec4Count, data, m_curVS->regSunDir, 1, m_sunDir);
    CopyRegRange(startReg, vec4Count, data, m_curVS->regSunCol, 1, m_sunCol);
    CopyRegRange(startReg, vec4Count, data, m_curVS->regSkyCol, 1, m_skyCol);
}
bool SceneTracker::FilterVSConstF(UINT startReg, const float* data, UINT vec4Count, float* outBuf) {
    if (!m_disableAmbient || !m_curVS || m_curVS->regSunCol < 0 || !data || vec4Count > 256) {
        return false;
    }
    UINT sc = static_cast<UINT>(m_curVS->regSunCol);
    if (sc < startReg || sc >= startReg + vec4Count) {
        return false;
    }
    memcpy(outBuf, data, vec4Count * 4 * sizeof(float));
    float* dst = outBuf + (sc - startReg) * 4;
    dst[0] = dst[1] = dst[2] = dst[3] = 0.0f;
    return true;
}

void SceneTracker::FillFrameHeader(ipc::FrameHeader& hdr) {
    memcpy(hdr.viewProj, m_camLatched ? m_latchedViewProj : m_viewProj, sizeof(m_viewProj));
    memcpy(hdr.invView, m_invView, sizeof(m_invView));
    memcpy(hdr.camPos, m_camPos, sizeof(m_camPos));
    memcpy(hdr.sunDir, m_sunDir, sizeof(m_sunDir));
    memcpy(hdr.sunColor, m_sunCol, sizeof(m_sunCol));
    memcpy(hdr.skyColor, m_skyCol, sizeof(m_skyCol));
}