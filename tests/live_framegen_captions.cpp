// Build live-framegen-caption-test and run build/demo/live-framegen-caption-test on an optical-flow GPU.
#include "live_framegen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace {
constexpr int width = 512, height = 288, top = 219;
using Frame = std::vector<uint8_t>;
using Mask = std::vector<uint8_t>;
using Glyph = std::array<unsigned, 7>;
constexpr Glyph glyphs[] = {
    {15, 16, 16, 14, 1, 1, 30},   // S
    {17, 17, 17, 17, 17, 17, 14}, // U
    {30, 17, 17, 30, 17, 17, 30}, // B
    {31, 4, 4, 4, 4, 4, 4},       // T
    {14, 4, 4, 4, 4, 4, 14},      // I
    {16, 16, 16, 16, 16, 16, 31}, // L
    {31, 16, 16, 30, 16, 16, 31}, // E
};

Mask textMask(int origin, int variant) {
    Mask mask(width * height);
    // Single-pixel stems over a panning scene. Stretch vertically for tall subtitle glyphs.
    for (int i = 0; i < 16; ++i) {
        const auto& glyph = glyphs[(i + variant) % std::size(glyphs)];
        for (int y = 0; y < 14; ++y)
            for (int x = 0; x < 5; ++x)
                mask[(top + y) * width + origin + i * 8 + x] = (glyph[y / 2] >> (4 - x)) & 1u;
    }
    return mask;
}

uint32_t noise(unsigned x, unsigned y) {
    uint32_t n = x * 0x85ebca6bu + y * 0x9e3779b9u;
    n ^= n >> 16;
    n *= 0xc2b2ae35u;
    return n ^ (n >> 13);
}

Frame render(int pan, const Mask& mask, int ink = 242) {
    Frame result(width * height * 4, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned worldX = x + pan;
            const uint32_t base = noise(worldX / 4, y / 4), detail = noise(worldX, y);
            const int i = y * width + x;
            for (int c = 0; c < 3; ++c) {
                int background = 25 + ((base >> (8 * c)) & 95) + ((detail >> (8 * c)) & 31);
                result[4 * i + c] = mask[i] ? ink : background;
            }
        }
    }
    return result;
}

struct Error {
    double total = 0;
    unsigned count = 0;
    void add(int actual, int expected) {
        total += std::abs(actual - expected);
        ++count;
    }
    double mean() const { return total / count; }
};

void steadyCaption(LiveFrameGen& fg, const Mask& caption, bool enhanced) {
    Error strokes, background, heldBackground;
    unsigned ghosts = 0, gaps = 0;
    Frame mid(width * height * 4);
    for (int offset = 0; offset < 64; offset += 8) {
        auto rawA = render(offset, caption), rawB = render(offset + 8, caption);
        // Mimic NR changing caption exposure slightly between source endpoints.
        auto a = enhanced ? render(offset, caption, 219) : rawA;
        auto b = enhanced ? render(offset + 8, caption, 225) : rawB;
        auto expected = render(offset + 4, caption, enhanced ? 222 : 242);
        if (!fg.interpolate(a.data(), b.data(), mid.data(), enhanced ? rawA.data() : nullptr,
                            enhanced ? rawB.data() : nullptr))
            throw std::runtime_error("subtitle on continuous camera pan dropped a midpoint");
        for (int y = 24; y < height - 24; ++y) {
            for (int x = 24; x < width - 24; ++x) {
                int pixel = y * width + x;
                for (int c = 0; c < 3; ++c) {
                    int p = 4 * pixel + c;
                    if (caption[pixel]) strokes.add(mid[p], expected[p]);
                    else if (y >= top && y < top + 14 && x >= 110 && x < 254) {
                        // The scene's brightest possible channel is 151. A brighter pixel in a
                        // glyph gap can only be displaced subtitle ink.
                        ++gaps;
                        if (mid[p] > 180) ++ghosts;
                    } else if (y < top - 16) {
                        background.add(mid[p], expected[p]);
                        heldBackground.add(a[p], expected[p]);
                    }
                }
            }
        }
    }
    const double ghostFraction = double(ghosts) / gaps;
    std::printf("%s subtitle RGB error %.2f; misplaced ink %.1f%%; moving background %.2f (held endpoint %.2f)\n",
                enhanced ? "separate source/display" : "capture", strokes.mean(), 100 * ghostFraction,
                background.mean(), heldBackground.mean());
    if (strokes.mean() > 8 || ghostFraction > 0.05 || background.mean() > heldBackground.mean() * 0.4)
        throw std::runtime_error("midpoint must preserve subtitle strokes and interpolate the background");
}

void changedCaption(LiveFrameGen& fg, const Mask& oldLine, const Mask& newLine) {
    const auto a = render(0, oldLine), b = render(8, newLine);
    Frame mid(width * height * 4);
    if (!fg.interpolate(a.data(), b.data(), mid.data())) return; // Holding a real frame across a cue change is valid.
    Error oldCue, newCue;
    for (int i = 0; i < width * height; ++i) {
        if (oldLine[i] == newLine[i]) continue;
        for (int c = 0; c < 3; ++c) {
            // A held or interpolated background is valid. Its channels are all <= 151.
            int channel = mid[4 * i + c];
            oldCue.add(channel, oldLine[i] ? 242 : std::min(151, channel));
            newCue.add(channel, newLine[i] ? 242 : std::min(151, channel));
        }
    }
    std::printf("cue transition ink error to prior %.2f / next %.2f\n", oldCue.mean(), newCue.mean());
    if (std::min(oldCue.mean(), newCue.mean()) > 15)
        throw std::runtime_error("midpoint displayed a partial subtitle cue");
}

// Lettering on a moving scene, e.g. a sign in the video, should follow its optical flow.
void movingText(LiveFrameGen& fg) {
    const auto before = textMask(118, 0), middle = textMask(114, 0), after = textMask(110, 0);
    const auto a = render(0, before), b = render(8, after);
    Frame mid(width * height * 4);
    if (!fg.interpolate(a.data(), b.data(), mid.data()))
        throw std::runtime_error("dropped a midpoint during a pan over moving text");
    Error strokes;
    for (int i = 0; i < width * height; ++i) {
        if (middle[i])
            for (int c = 0; c < 3; ++c) strokes.add(mid[4 * i + c], 242);
    }
    std::printf("moving text RGB error at its halfway position %.2f\n", strokes.mean());
    if (strokes.mean() > 8) throw std::runtime_error("moving text failed to reach its halfway position");
}

// Parallax reveals and hides a textured wall behind a nearer row of bright slats.
// Rendering the scene at the half step gives exact ground truth, including
// legitimate highlights that appear only between the two input frames.
Frame renderParallax(int step) {
    Frame pixels(width * height * 4, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int wallX = x + 219 + 2 * step, wallY = y + 32;
            const int slatX = x + 94 - 8 * step;
            const bool slat = (slatX + y / 6) % 40 < 17;
            const unsigned wall = noise(wallX / 5, wallY / 4), grain = noise(wallX, wallY);
            const unsigned metal = noise(slatX / 3, y / 4);
            for (unsigned c = 0; c < 3; ++c) {
                const unsigned shift = 8 * c;
                pixels[4 * (y * width + x) + c] = slat ? 165 + ((metal >> shift) % 70)
                    : 38 + ((wall >> shift) & 127) + ((grain >> shift) & 15);
            }
        }
    }
    return pixels;
}

int luma(const Frame& frame, int index) {
    const auto* rgb = &frame[4 * index];
    return (54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2] + 128) / 256;
}

void movingOcclusion(LiveFrameGen& fg) {
    Error interpolated, held;
    unsigned flashes = 0, samples = 0, accepted = 0;
    Frame mid(width * height * 4);
    for (int step = 0; step < 10; step += 2) {
        const Frame a = renderParallax(step), reference = renderParallax(step + 1), b = renderParallax(step + 2);
        if (!fg.interpolate(a.data(), b.data(), mid.data())) continue;
        ++accepted;
        for (int y = 50; y < height - 35; ++y) {
            for (int x = 32; x < width - 32; ++x) {
                const int i = y * width + x;
                const int first = luma(a, i), second = luma(b, i), guess = luma(mid, i), expected = luma(reference, i);
                const auto excess = [&](int value) {
                    return std::max({0, std::min(first, second) - value, value - std::max(first, second)});
                };
                flashes += excess(guess) > excess(expected) + 20;
                ++samples;
                interpolated.add(guess, expected);
                held.add(first, expected);
            }
        }
    }
    if (accepted < 3) throw std::runtime_error("too few parallax midpoints to test motion stability");
    const double pulseRate = double(flashes) / samples;
    std::printf("moving occlusions: %.2f%% false luminance pulses, midpoint error %.2f (held %.2f)\n",
                100 * pulseRate, interpolated.mean(), held.mean());
    if (pulseRate > 0.025 || interpolated.mean() > 0.7 * held.mean())
        throw std::runtime_error("generated frames must retain motion without flashing at occlusions");
}

// A softly focused wheel crosses a strip of moving pavement. The rest of the
// screen is still, so frame-wide quality averages can hide a torn wheel.
Frame renderWheel(int offset) {
    Frame pixels(width * height * 4, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int u = x - 240 - offset, v = y - 145;
            const double radius = std::hypot(u, v);
            const double road = 123 + 48 * std::sin((x + offset * .22) * .084 + y * .011)
                                    + 51 * std::cos(y * .12 + x * .026 - offset * .037);
            const double wheel = 70 + 37 * std::sin(radius * .16) + 17 * std::cos(u * .07 - v * .13);
            const double cover = std::clamp((63 - radius) / 7, 0., 1.);
            const double strip = std::clamp(std::min(145 - std::abs(x - 256), 83 - std::abs(y - 147)) / 12., 0., 1.);
            const int level = int(std::lround(124 + strip * (road + cover * (wheel - road) - 124)));
            std::fill_n(&pixels[4 * (y * width + x)], 3, level);
        }
    }
    return pixels;
}

void wheelCrossing(LiveFrameGen& fg) {
    const Frame reference = renderWheel(0);
    Frame mid(width * height * 4);
    // After the fast crossing, a slow crossing must still produce an accurate midpoint.
    for (int offset : {48, 2}) {
        const Frame a = renderWheel(-offset), b = renderWheel(offset);
        if (!fg.interpolate(a.data(), b.data(), mid.data())) {
            if (offset == 2) throw std::runtime_error("dropped a trackable wheel after the fast crossing");
            std::puts("fast wheel crossing held at source cadence");
            continue;
        }
        unsigned seams = 0, count = 0;
        Error predicted, held;
        for (int y = 77; y <= 217; ++y) {
            for (int x = 124; x <= 389; ++x) {
                const int i = y * width + x;
                // The endpoints and true midpoint have no steps above 32 levels
                // anywhere in this strip. Such steps in a warp are false seams.
                const auto edge = [i](const Frame& image) {
                    const int center = image[4 * i];
                    return std::max(std::abs(center - image[4 * (i - 1)]),
                                    std::abs(center - image[4 * (i - width)]));
                };
                if (std::max({edge(a), edge(reference), edge(b)}) > 32)
                    throw std::runtime_error("wheel fixture contains an unexpected hard edge");
                seams += edge(mid) > 32;
                ++count;
                predicted.add(mid[4 * i], reference[4 * i]);
                held.add(a[4 * i], reference[4 * i]);
            }
        }
        const double seamRate = double(seams) / count;
        std::printf("wheel half-step %d px: %.2f%% false seams, midpoint error %.2f (held %.2f)\n",
                    offset, 100 * seamRate, predicted.mean(), held.mean());
        if (seamRate > .005) throw std::runtime_error("inserted a wheel with sharp, broken contours");
        if (offset == 2 && predicted.mean() > .6 * held.mean())
            throw std::runtime_error("trackable wheel failed to reach its midpoint");
    }
}
} // namespace

int main() {
    try {
        LiveFrameGen fg(width, height);
        const auto caption = textMask(118, 0), next = textMask(234, 4);
        const Mask empty(width * height);
        steadyCaption(fg, caption, false);
        steadyCaption(fg, caption, true);
        changedCaption(fg, empty, caption);
        changedCaption(fg, caption, next);
        changedCaption(fg, caption, empty);
        movingText(fg);
        movingOcclusion(fg);
        wheelCrossing(fg);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return EXIT_FAILURE;
    }
}
