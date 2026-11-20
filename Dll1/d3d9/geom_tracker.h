#pragma once
#include <d3d9.h>
#include <cstdint>
#include <vector>
#include "../../shared/ipc_protocol.h"

class GeomTracker {
public:
    void BeginFrame();
    void OnDrawIndexedPrim(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
        INT baseV, UINT minV, UINT numV, UINT startIdx, UINT primCount);
    void FillInstances(ipc::SharedMemoryLayout* shm);
    void ClearCache();

private:
    bool UploadMeshIfNew(IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib,
        UINT vbOffset, UINT stride, INT baseV, UINT numV, UINT startIdx, UINT primCount,
        uint64_t meshId);

    std::vector<ipc::InstanceDesc> m_frameInstances;
};

extern GeomTracker g_geom;