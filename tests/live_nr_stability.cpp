// From the repository root, build live-nr-stability-test and run build/demo/live-nr-stability-test.
// Uses the same model, Vulkan graph and shader files as the live viewer.
#include "live_nr.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr int w = 288, h = 256;
using Frame = std::vector<uint8_t>;

void crop(Frame& input, const uint8_t* gallery, int stride, int step) {
  for (int y = 0; y < h; ++y) {
    const auto* scanline = gallery + 4 * ((270 + y) * stride + 345 + step);
    std::memcpy(input.data() + 4 * w * y, scanline, w * 4);
  }
}

double panError(LiveNr& nr, const uint8_t* gallery, int stride, bool spatialOnly) {
  nr.resetHistory();
  Frame input(w * h * 4), previous;
  double total = 0;
  size_t samples = 0;
  for (int step = 0; step <= 16; ++step) {
    crop(input, gallery, stride, step);
    if (spatialOnly) nr.resetHistory();
    const auto* current = nr.process(input.data());
    if (!previous.empty()) {
      // Follow the same eyes, nose and cheeks through the one-pixel camera pan.
      for (int y = 36; y < 190; ++y) {
        for (int x = 76 - step; x < 205 - step; ++x) {
          size_t i = 4 * (y * w + x);
          for (int c = 0; c < 3; ++c) {
            total += std::abs(int(current[i + c]) - int(previous[i + 4 + c]));
            ++samples;
          }
        }
      }
    }
    previous.assign(current, current + input.size());
  }
  if (!std::equal(previous.begin(), previous.end(), nr.process(input.data())))
    throw std::runtime_error("an identical source produced a different image after the pan stopped");
  return total / samples;
}
} // namespace

int main() {
  try {
    int width, height, channels;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> gallery(
        stbi_load("docs/images/comparisons/character-views.png", &width, &height, &channels, 4), stbi_image_free);
    if (!gallery || width < 345 + w + 16 || height < 270 + h)
      throw std::runtime_error("could not read the front portrait in the README gallery");
    LiveNr nr(w, h, "models/nr", "build/shaders", "build/data/shaders");
    const double spatial = panError(nr, gallery.get(), width, true);
    const double temporal = panError(nr, gallery.get(), width, false);
    std::printf("Mean face RGB variation per frame (0-255): spatial %.3f, stabilized %.3f\n", spatial, temporal);
    if (spatial < 0.2 || temporal > 0.5 * spatial)
      throw std::runtime_error("motion-aligned face variation did not fall by at least half");

    Frame overlay(w * h * 4);
    crop(overlay, gallery.get(), width, 16);
    // A newly exposed overlay covers the eye region while most of the source stays put.
    for (int y = 72; y < 112; ++y) {
      for (int x = 100; x < 148; ++x) {
        const size_t i = 4 * (y * w + x);
        overlay[i] = overlay[i + 2] = 0;
        overlay[i + 1] = 255;
      }
    }
    const auto* covered = nr.process(overlay.data());
    Frame withHistory(covered, covered + overlay.size());
    nr.resetHistory();
    const auto* fresh = nr.process(overlay.data());
    for (int y = 72; y < 112; ++y) {
      const size_t i = 4 * (y * w + 100);
      if (!std::equal(withHistory.begin() + i, withHistory.begin() + i + 48 * 4, fresh + i))
        throw std::runtime_error("a new overlay retained detail from the face");
    }

    Frame cut(w * h * 4, 255);
    const auto* afterCut = nr.process(cut.data());
    Frame reference(afterCut, afterCut + cut.size());
    nr.resetHistory();
    if (!std::equal(reference.begin(), reference.end(), nr.process(cut.data())))
      throw std::runtime_error("a hard cut carried detail from the previous source");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
