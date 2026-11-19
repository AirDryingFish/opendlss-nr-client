#include "live_mako.h"
#include "live_fg_plugin.h"

namespace {
constexpr char library[] = "lib/liblive_mako.so";
constexpr char model[] = "data/mako-fg-v1.24/model";
}

bool LiveMako::available() {
    return LiveFgPlugin::installed(library, model, {"weights.bin", "ops.spv", "transport.spv", "mixture.spv"});
}

LiveMako::LiveMako(unsigned width, unsigned height, unsigned multiplier)
    : plugin_(std::make_unique<LiveFgPlugin>(library, "mako", model, width, height, multiplier)) {}
LiveMako::~LiveMako() = default;

bool LiveMako::interpolate(const uint8_t* previous, const uint8_t* current, uint8_t* intermediates) {
    return plugin_->interpolate(previous, current, intermediates);
}
