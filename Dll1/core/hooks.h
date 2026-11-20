#pragma once
#include <cstdio>
#include "MinHook.h"
#include "log.h"

void InitHooks();
void ShutdownHooks();

template <typename T>
inline void HookVtable(void* iface, size_t index, T newFn, T* origFn) {
    void** vtable = *reinterpret_cast<void***>(iface);
    void* target = vtable[index];
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(newFn), reinterpret_cast<void**>(origFn));
    if (st == MH_OK) {
        MH_EnableHook(target);
    }
    else if (st != MH_ERROR_ALREADY_CREATED) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[ERR] MH_CreateHook idx=%zu target=%p failed (%d)", index, target, (int)st);
        LogLine(buf);
    }
}