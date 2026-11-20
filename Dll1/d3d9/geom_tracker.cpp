#include "geom_tracker.h"
#include "scene_tracker.h"
#include "ipc_client.h"
#include "../core/log.h"
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <cmath>

GeomTracker g_geom;

static constexpr uint32_t MAX_UPLOADS_PER_FRAME = 8;
static uint32_t g_uploadsThisFrame = 0;

static std::unordered_set<uint64_t> g_uploadedMeshes;
static std::unordered_map<IDirect3DVertexDeclaration9*, bool> g_declSkinnedCache;
static uint32_t g_cntOk = 0, g_cntNoWorld = 0, g_cntNotTriList = 0, g_cntUploadFail = 0;
static uint32_t g_diagFrame = 0;

static bool IsSkinnedDraw(IDirect3DDevice9* dev) {
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(dev->GetVertexDeclaration(&decl)) || !decl) return false;

    auto it = g_declSkinnedCache.find(decl);
    if (it != g_declSkinnedCache.end()) {
        decl->Release();
        return it->second;
    }

    D3DVERTEXELEMENT9 elems[32]{};
    UINT numElems = 32;
    bool skinned = false;
    if (SUCCEEDED(decl->GetDeclaration(elems, &numElems))) {
        for (UINT i = 0; i < numElems; ++i) {
            if (elems[i].Stream == 0xFF) break; // D3DDECL_END
            if (elems[i].Usage == D3DDECLUSAGE_BLENDWEIGHT ||
                elems[i].Usage == D3DDECLUSAGE_BLENDINDICES) {
                skinned = true;
                break;
            }
        }
    }
    g_declSkinnedCache[decl] = skinned;
    decl->Release();
    return skinned;
}

static uint64_t MakeMeshId(void* vb, void* ib, UINT vbOffset, INT baseV, UINT startIdx, UINT primCount) {
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
    mix(reinterpret_cast<uintptr_t>(vb));
    mix(reinterpret_cast<uintptr_t>(ib));
    mix(vbOffset);
    mix(static_cast<uint64_t>(baseV));
    mix(startIdx);
    mix(primCount);
    return h;
}

void GeomTracker::BeginFrame() {
    g_scene.BeginFrame();
    g_uploadsThisFrame = 0;
    if (++g_diagFrame % 120 == 0) {
        LogLine("[GEOM] ok=" + std::to_string(g_cntOk) +
            " noWorld=" + std::to_string(g_cntNoWorld) +
            " notTriList=" + std::to_string(g_cntNotTriList) +
            " uploadFail=" + std::to_string(g_cntUploadFail));
    }
    g_cntOk = g_cntNoWorld = g_cntNotTriList = g_cntUploadFail = 0;
    m_frameInstances.clear();
}

void GeomTracker::ClearCache() {
    g_uploadedMeshes.clear();
    g_declSkinnedCache.clear();
    m_frameInstances.clear();
}

bool GeomTracker::UploadMeshIfNew(
    IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib,
    UINT vbOffset, UINT stride, INT baseV, UINT numV, UINT startIdx, UINT primCount,
    uint64_t meshId)
{
    if (g_uploadedMeshes.count(meshId)) return true;
    if (g_uploadsThisFrame >= MAX_UPLOADS_PER_FRAME) return false;
    if (!g_ipc.IsReady() || stride < 12) return false;

    uint32_t idxCount = primCount * 3;
    if (numV == 0 || idxCount == 0 || numV > ipc::MAX_MESH_VERTS || idxCount > ipc::MAX_MESH_INDXS) {
        return false;
    }

    D3DVERTEXBUFFER_DESC vbDesc{};
    D3DINDEXBUFFER_DESC  ibDesc{};
    if (FAILED(vb->GetDesc(&vbDesc)) || FAILED(ib->GetDesc(&ibDesc))) return false;
    if (vbOffset >= vbDesc.Size) return false;

    bool is32 = (ibDesc.Format == D3DFMT_INDEX32);
    UINT idxSize = is32 ? 4 : 2;
    if ((startIdx + idxCount) * idxSize > ibDesc.Size) return false;

    void* vbPtr = nullptr;
    void* ibPtr = nullptr;
    if (FAILED(vb->Lock(0, 0, &vbPtr, D3DLOCK_READONLY))) return false;
    if (FAILED(ib->Lock(startIdx * idxSize, idxCount * idxSize, &ibPtr, D3DLOCK_READONLY))) {
        vb->Unlock();
        return false;
    }

    auto* shm = g_ipc.Data();
    const uint8_t* rawV = static_cast<const uint8_t*>(vbPtr) + vbOffset;
    uint32_t maxVertInBuf = (vbDesc.Size - vbOffset) / stride;

    // Читаем индексы и приводим к [0..numV-1] с учётом baseV
    uint32_t minIdx = UINT32_MAX, maxIdx = 0;
    for (uint32_t i = 0; i < idxCount; ++i) {
        uint32_t rawIdx = is32
            ? static_cast<const uint32_t*>(ibPtr)[i]
            : static_cast<const uint16_t*>(ibPtr)[i];
        int32_t v = static_cast<int32_t>(rawIdx) + baseV;
        if (v < 0 || static_cast<uint32_t>(v) >= maxVertInBuf) {
            ib->Unlock();
            vb->Unlock();
            return false;
        }
        uint32_t uv = static_cast<uint32_t>(v);
        if (uv < minIdx) minIdx = uv;
        if (uv > maxIdx) maxIdx = uv;
        shm->meshIndices[i] = uv;
    }

    uint32_t usedVerts = maxIdx - minIdx + 1;
    if (usedVerts > ipc::MAX_MESH_VERTS) {
        ib->Unlock();
        vb->Unlock();
        return false;
    }

    // Нормализуем индексы от 0 и копируем только валидные xyz (защита от NaN/Inf -> TDR)
    for (uint32_t i = 0; i < idxCount; ++i) {
        shm->meshIndices[i] -= minIdx;
    }
    for (uint32_t i = 0; i < usedVerts; ++i) {
        const float* pos = reinterpret_cast<const float*>(rawV + (minIdx + i) * stride);
        if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]) ||
            std::fabs(pos[0]) > 1e6f || std::fabs(pos[1]) > 1e6f || std::fabs(pos[2]) > 1e6f) {
            ib->Unlock();
            vb->Unlock();
            return false;
        }
        shm->meshVertices[i * 3 + 0] = pos[0];
        shm->meshVertices[i * 3 + 1] = pos[1];
        shm->meshVertices[i * 3 + 2] = pos[2];
    }

    ib->Unlock();
    vb->Unlock();

    shm->header.command = static_cast<uint32_t>(ipc::Command::UploadMesh);
    shm->meshUpload.meshId = meshId;
    shm->meshUpload.vertexCount = usedVerts;
    shm->meshUpload.indexCount = idxCount;

    if (g_ipc.SubmitFrameAndWait(2000)) {
        g_uploadedMeshes.insert(meshId);
        g_uploadsThisFrame++;
        return true;
    }
    return false;
}

void GeomTracker::OnDrawIndexedPrim(
    IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
    INT baseV, UINT minV, UINT numV, UINT startIdx, UINT primCount)
{
    if (!g_scene.IsMainPass() || g_scene.IsCurrentSkinned() || IsSkinnedDraw(dev)) return;

    DWORD zWrite = TRUE, alphaBlend = FALSE, alphaTest = FALSE;
    dev->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
    if (!zWrite || alphaBlend || alphaTest) return;

    if (type != D3DPT_TRIANGLELIST) { g_cntNotTriList++; return; }

    bool hasWorld = g_scene.HasWorldMatrix();
    if (!hasWorld && !g_scene.HasViewProj()) {
        g_cntNoWorld++;
        return;
    }
    if (g_scene.HasViewProj()) g_scene.LatchMainCamera();
    if (m_frameInstances.size() >= ipc::MAX_INSTANCES) return;

    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    UINT offset = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(0, &vb, &offset, &stride)) || !vb) return;
    if (FAILED(dev->GetIndices(&ib)) || !ib) { vb->Release(); return; }

    uint64_t meshId = MakeMeshId(vb, ib, offset, baseV, startIdx, primCount);
    if (UploadMeshIfNew(vb, ib, offset, stride, baseV, numV, startIdx, primCount, meshId)) {
        ipc::InstanceDesc inst{};
        inst.meshId = meshId;
        if (hasWorld) {
            memcpy(inst.world4x3, g_scene.GetWorldMatrix(), sizeof(inst.world4x3));
        }
        else {
            static constexpr float kIdentity4x3[12] = {
                1.f, 0.f, 0.f, 0.f,
                0.f, 1.f, 0.f, 0.f,
                0.f, 0.f, 1.f, 0.f
            };
            memcpy(inst.world4x3, kIdentity4x3, sizeof(inst.world4x3));
        }

        bool matOk = true;
        for (int k = 0; k < 12; ++k) {
            if (!std::isfinite(inst.world4x3[k]) || std::fabs(inst.world4x3[k]) > 1e6f) {
                matOk = false;
                break;
            }
        }
        if (matOk) {
            m_frameInstances.push_back(inst);
            g_cntOk++;
        }
    }
    else {
        g_cntUploadFail++;
    }

    ib->Release();
    vb->Release();
}

void GeomTracker::FillInstances(ipc::SharedMemoryLayout* shm) {
    uint32_t count = static_cast<uint32_t>(m_frameInstances.size());
    if (count > ipc::MAX_INSTANCES) count = ipc::MAX_INSTANCES;
    shm->header.instanceCount = count;
    if (count > 0) {
        memcpy(shm->instances, m_frameInstances.data(), count * sizeof(ipc::InstanceDesc));
    }
}