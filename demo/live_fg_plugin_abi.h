#pragma once

#include <stddef.h>
#include <stdint.h>

// C-only boundary between the Filament client and the optional Vulkan backends.
struct LiveFgPluginV1 {
    void* (*open)(unsigned width, unsigned height, const char* model, char* error, size_t errorSize);
    int (*midpoint)(void* session, const uint8_t* previous, const uint8_t* current, uint8_t* output,
                    char* error, size_t errorSize);
    void (*close)(void* session);
};

using LiveFgPluginQuery = const LiveFgPluginV1* (*)();

// V2 opens one session per multiplier and returns every phase in a contiguous RGBA8 batch.
// V1 remains exported for older clients which allocate only a single intermediate frame.
struct LiveFgPluginV2 {
    void* (*open)(unsigned width, unsigned height, unsigned multiplier, const char* model,
                  char* error, size_t errorSize);
    int (*interpolate)(void* session, const uint8_t* previous, const uint8_t* current,
                       uint8_t* intermediates, char* error, size_t errorSize);
    void (*close)(void* session);
};

using LiveFgPluginQueryV2 = const LiveFgPluginV2* (*)();
