#pragma once
#include <cstdint>
#include <memory>

class LiveFgPlugin;
class LiveRife {
public:
    static bool available();
    LiveRife(unsigned width, unsigned height, unsigned multiplier = 2);
    ~LiveRife();

    // Output holds multiplier - 1 consecutive RGBA8 frames.
    bool interpolate(const uint8_t* previous, const uint8_t* current, uint8_t* intermediates);

private:
    std::unique_ptr<LiveFgPlugin> plugin_;
};
