#include "hooks.h"

void InitHooks() {
    MH_Initialize();
}

void ShutdownHooks() {
    MH_Uninitialize();
}