#pragma once
#include <d3d9.h>
#include <cstdint>
#include "ctab_parser.h"
#include "../../shared/ipc_protocol.h"

struct VsSlotMap {
    bool isSkinned = false;
    int16_t regViewProj = -1;
    int16_t regWorld = -1;
    int16_t regInvView = -1;
    int16_t regCamPos = -1;
    int16_t regSunDir = -1;
    int16_t regSunCol = -1;
    int16_t regSkyCol = -1;
};

struct PsSlotMap {
    int16_t  regSH = -1;
    int16_t  regDirLight = -1;
    int16_t  regRimLight = -1;
    int16_t  regShadowDensity = -1;
    int8_t   diffuseSlot = -1;
};

class SceneTracker {
public:
    void RegisterVS(IDirect3DVertexShader9* vs, const ShaderInfo& info);
    void RegisterPS(IDirect3DPixelShader9* ps, const ShaderInfo& info, int diffuseSlot);
    void OnSetVS(IDirect3DVertexShader9* vs);
    void OnSetPS(IDirect3DPixelShader9* ps);

    void OnSetVSConstF(UINT startReg, const float* data, UINT vec4Count);
    // Возвращает true, если нужно подменить константы (для обнуления arrSHCofficients)
    bool FilterPSConstF(UINT startReg, const float* data, UINT vec4Count, float* outBuf);
    bool FilterVSConstF(UINT startReg, const float* data, UINT vec4Count, float* outBuf);
    void LatchMainCamera();
    void BeginFrame() { m_camLatched = false; }
    void FillFrameHeader(ipc::FrameHeader& hdr);
    void ToggleDisableAmbient() { m_disableAmbient = !m_disableAmbient; }
    bool IsAmbientDisabled() const { return m_disableAmbient; }
    void SetMainPass(bool isMain) { m_isMainPass = isMain; }
    bool IsMainPass() const { return m_isMainPass; }
    bool IsCurrentSkinned() const { return m_curVS && m_curVS->isSkinned; }
    bool HasWorldMatrix() const { return m_hasWorld; }
    const float* GetWorldMatrix() const { return m_world; }
    bool HasViewProj() const { return m_curVS && m_curVS->regViewProj >= 0; }

    bool IsWorldLitPS() const {
        return m_curPS && (m_curPS->regSH >= 0 || m_curPS->regDirLight >= 0);
    }
    int  GetDiffuseSlot() const { return m_curPS ? m_curPS->diffuseSlot : -1; }
private:
    const VsSlotMap* m_curVS = nullptr;
    const PsSlotMap* m_curPS = nullptr;
    float m_latchedViewProj[16]{};
    bool  m_camLatched = false;
    bool m_isMainPass = true;
    bool m_disableAmbient = false;
    float m_world[12]{};
    bool  m_hasWorld = false;
    float m_viewProj[16]{};
    float m_invView[12]{};
    float m_camPos[4]{};
    float m_sunDir[4]{};
    float m_sunCol[4]{};
    float m_skyCol[4]{};
};

extern SceneTracker g_scene;