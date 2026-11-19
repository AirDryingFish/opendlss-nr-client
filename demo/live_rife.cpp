#include "live_rife.h"
#include "live_fg_plugin.h"

namespace {
constexpr char library[] = "lib/liblive_rife.so";
constexpr char model[] = "data/rife-v4.6";
}

bool LiveRife::available() {
    return LiveFgPlugin::installed(library, model, {"flownet.param", "flownet.bin"});
}

LiveRife::LiveRife(unsigned width, unsigned height, unsigned multiplier)
    : plugin_(std::make_unique<LiveFgPlugin>(library, "rife", model, width, height, multiplier)) {}
LiveRife::~LiveRife() = default;

bool LiveRife::interpolate(const uint8_t* previous, const uint8_t* current, uint8_t* intermediates) {
    return plugin_->interpolate(previous, current, intermediates);
}
