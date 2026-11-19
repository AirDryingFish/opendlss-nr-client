// Opt-in GPU check: run live-multipliers-test flow, rife or mako from build/demo.
#include "live_framegen.h"
#include "live_mako.h"
#include "live_rife.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
constexpr int width = 384, height = 216, travel = 12;
using Image = std::vector<uint8_t>;

// A band-limited, nonrepeating texture translating an integral number of pixels
// makes every output phase independently measurable, without another interpolator.
double grain(int x, int y, int channel) {
    uint32_t seed = uint32_t(x) * 0x85ebca6bu + uint32_t(y) * 0xc2b2ae35u + channel * 0x9e3779b9u;
    seed ^= seed >> 16;
    seed *= 0x85ebca6bu;
    return double((seed ^ (seed >> 13)) & 255u);
}

double texture(int u, int y, int spacing, int channel) {
    const int ix = u / spacing, iy = y / spacing;
    const double fx = double(u % spacing) / spacing, fy = double(y % spacing) / spacing;
    return std::lerp(std::lerp(grain(ix, iy, channel), grain(ix + 1, iy, channel), fx),
                     std::lerp(grain(ix, iy + 1, channel), grain(ix + 1, iy + 1, channel), fx), fy);
}

Image scene(int offset) {
    Image frame(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int u = x + 96 - offset;
            for (int c = 0; c < 3; ++c) {
                const double v = .45 * texture(u, y, 5, c) + .30 * texture(u, y, 13, c) + .25 * texture(u, y, 27, c);
                frame[4 * (y * width + x) + c] = uint8_t(std::round(v));
            }
            frame[4 * (y * width + x) + 3] = 255;
        }
    }
    return frame;
}

double alignmentError(const uint8_t* frame, const Image& first, int shift) {
    double error = 0;
    for (int y = 18; y < height - 18; y += 2)
        for (int x = travel + 14; x < width - 14; x += 2)
            for (int c = 0; c < 3; ++c) {
                const int i = (y * width + x) * 4 + c;
                error += std::abs(int(frame[i]) - int(first[i - shift * 4]));
            }
    return error;
}

template<class Model>
void verify(const char* name) {
    for (int multiplier : {2, 3, 4}) {
        Model model(width, height, multiplier);
        auto previous = scene(0);
        std::vector<uint8_t> generated(previous.size() * (multiplier - 1));
        for (int pair = 1; pair <= 3; ++pair) {
            auto current = scene(pair * travel);
            if (!model.interpolate(previous.data(), current.data(), generated.data()))
                throw std::runtime_error("dropped a trackable pair");
            for (size_t i = 3; i < generated.size(); i += 4)
                if (generated[i] != 255) throw std::runtime_error("generated batch contains a non-opaque or unwritten pixel");
            for (int phase = 1; phase < multiplier; ++phase) {
                const auto* pixels = generated.data() + (phase - 1) * previous.size();
                const int expected = phase * travel / multiplier;
                int best = 0;
                double bestError = alignmentError(pixels, previous, best);
                for (int pos = 1; pos <= travel; ++pos) {
                    const double error = alignmentError(pixels, previous, pos);
                    if (error < bestError) { bestError = error; best = pos; }
                }
                if (std::abs(best - expected) > 1 ||
                    bestError > .60 * std::min(alignmentError(pixels, previous, 0),
                                               alignmentError(pixels, previous, travel)))
                    throw std::runtime_error("generated phase does not match the reference translation");
                if (pair == 1) std::printf("%s %dx phase %d/%d: %d px, reference %d px\n",
                                           name, multiplier, phase, multiplier, best, expected);
            }
            previous.swap(current);
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("choose a backend: flow, rife or mako");
        const std::string_view backend(argv[1]);
        if (backend == "flow") verify<LiveFrameGen>(argv[1]);
        else if (backend == "rife") verify<LiveRife>(argv[1]);
        else if (backend == "mako") verify<LiveMako>(argv[1]);
        else throw std::invalid_argument("unknown backend");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
