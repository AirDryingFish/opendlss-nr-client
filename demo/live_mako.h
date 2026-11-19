#pragma once
#include <cstdint>
#include <memory>

class LiveFgPlugin;
class LiveMako {
public:
    static bool available();
    LiveMako(unsigned width, unsigned height, unsigned multiplier = 2);
    ~LiveMako();

    // Output holds multiplier - 1 consecutive RGBA8 frames.
    bool interpolate(const uint8_t* previous, const uint8_t* current, uint8_t* intermediates);

private:
    std::unique_ptr<LiveFgPlugin> plugin_;
};
