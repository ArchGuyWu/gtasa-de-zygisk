#pragma once

#include <cstdint>

struct GtasaHost {
    uint32_t size;
    uintptr_t base;
    int (*hook)(void *target, void *detour, void **original);
};

extern "C" int gtasa_hook(void *target, void *detour, void **original);
