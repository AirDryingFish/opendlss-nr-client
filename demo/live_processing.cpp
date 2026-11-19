#include "live_processing.h"
#include "live_mako.h"
#include "live_nr.h"
#include "live_rife.h"
#include "live_rife_scene.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
// The Vulkan interpolators take two full-color endpoints. Check for cuts on the original
// captures so NR exposure changes cannot trigger a scene transition.
template<class Model>
class VideoInterpolator final : public LiveInterpolator {
public:
  VideoInterpolator(unsigned w, unsigned h, unsigned multiplier) : width(w), height(h), model(w, h, multiplier) {}
  bool interpolate(const uint8_t* previous, const uint8_t* current, uint8_t* midpoint,
                   const uint8_t* previousSource, const uint8_t* currentSource) override {
    const auto* a = previousSource ? previousSource : previous;
    const auto* b = currentSource ? currentSource : current;
    if (liveSceneCut(a, b, width, height) || rifeSceneCut(a, b, width, height)) return false;
    return model.interpolate(previous, current, midpoint);
  }
private:
  unsigned width, height;
  Model model;
};

} // namespace

std::unique_ptr<LiveInterpolator> makeLiveInterpolator(const std::string& backend, unsigned width,
                                                      unsigned height, unsigned multiplier) {
  if (backend == "rife") return std::make_unique<VideoInterpolator<LiveRife>>(width, height, multiplier);
  if (backend == "mako") return std::make_unique<VideoInterpolator<LiveMako>>(width, height, multiplier);
  if (backend == "flow") return std::make_unique<LiveFrameGen>(width, height, multiplier);
  throw std::invalid_argument("Unknown interpolation backend: " + backend);
}

void probeLiveProcessing(const std::string& feature, unsigned multiplier, const std::string& model,
                         const std::string& kernels, const std::string& shaders) {
  constexpr unsigned w = 320, h = 180;
  std::vector<uint8_t> a(w * h * 4, 255), b(a);
  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      const uint32_t cell = (x / 32 * 0x85ebca6bu) ^ (y / 30 * 0xc2b2ae35u);
      for (unsigned c = 0; c < 3; ++c) a[(y * w + x) * 4 + c] = 32 + ((cell >> (8 * c)) & 127) + ((x * 3 + y * 7) & 15);
    }
    for (unsigned x = 0; x < w; ++x)
      std::copy_n(&a[(y * w + (x + 8) % w) * 4], 4, &b[(y * w + x) * 4]);
  }
  if (feature == "nr") {
    LiveNr nr(w, h, model, kernels, shaders);
    const auto* output = nr.process(a.data());
    if (!output || std::equal(a.begin(), a.end(), output))
      throw std::runtime_error("NR probe did not produce an enhanced frame");
    std::printf("NR GPU inference OK on %s\n", nr.deviceName().c_str());
    return;
  }
  auto backend = makeLiveInterpolator(feature, w, h, multiplier);
  std::vector<uint8_t> batch(a.size() * (multiplier - 1));
  if (!backend->interpolate(a.data(), b.data(), batch.data(), nullptr, nullptr))
    throw std::runtime_error("Interpolation rejected a consistently moving test image");
  const uint8_t* prior = a.data();
  for (unsigned i = 0; i < multiplier - 1; ++i) {
    const auto* frame = batch.data() + i * a.size();
    if (frame[3] != 255 || std::equal(prior, prior + a.size(), frame) || std::equal(b.begin(), b.end(), frame))
      throw std::runtime_error("Interpolation did not return distinct RGBA frames");
    prior = frame;
  }
  std::printf("%s %ux GPU inference OK (%u intermediate frames)\n", feature.c_str(), multiplier, multiplier - 1);
}
