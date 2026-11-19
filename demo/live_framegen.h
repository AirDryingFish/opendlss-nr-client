#pragma once

#include <cstdint>
#include <memory>

bool liveSceneCut(const uint8_t* a, const uint8_t* b, unsigned width, unsigned height);

class LiveInterpolator {
public:
    virtual ~LiveInterpolator() = default;
    // On success, output holds multiplier - 1 contiguous RGBA8 frames, in presentation order.
    virtual bool interpolate(const uint8_t* previous, const uint8_t* current, uint8_t* intermediates,
                             const uint8_t* previousSource, const uint8_t* currentSource) = 0;
};

// Motion-compensated interpolation of two top-down, tightly packed RGBA8 images.
// The constructor reports unsupported build/driver/dimensions via std::runtime_error.
class LiveFrameGen final : public LiveInterpolator {
public:
    LiveFrameGen(unsigned width, unsigned height, unsigned multiplier = 2);
    ~LiveFrameGen() override;
    // Supply the original captures when the displayed endpoints have neural enhancement.
    // On a cut, unstable exposure or fractured motion, skip the whole batch.
    bool interpolate(const uint8_t* previous, const uint8_t* current, uint8_t* intermediates,
                     const uint8_t* previousSource = nullptr, const uint8_t* currentSource = nullptr) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
