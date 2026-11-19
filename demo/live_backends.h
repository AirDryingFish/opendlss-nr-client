#pragma once

#include "live_mako.h"
#include "live_rife.h"

#include <array>

struct LiveBackend {
    const char* id;
    const char* label;
    bool installed;
};

// Query installed files only. The chooser must not initialize a GPU session.
inline std::array<LiveBackend, 3> liveBackends() {
#ifdef LIVE_NVIDIA_FLOW
    constexpr bool flow = true;
#else
    constexpr bool flow = false;
#endif
    return {{{"rife", "RIFE v4.6 (Vulkan)", LiveRife::available()},
             {"mako", "MAKO FG (Vulkan)", LiveMako::available()},
             {"flow", "NVIDIA optical flow", flow}}};
}
