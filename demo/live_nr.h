#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Native FP8 NR for a fixed-size sRGB capture, with motion-matched stabilization of its residual.
class LiveNr {
 public:
  LiveNr(uint32_t width, uint32_t height, const std::string& modelDir, const std::string& kernelDir,
         const std::string& demoShaderDir);
  ~LiveNr();

  // Both rasters are tight, top-down RGBA8 of the constructor dimensions. The returned pixels are
  // valid until the next call, or alias the supplied pixels while bypassed. Vulkan/model errors throw.
  const uint8_t* process(const uint8_t* rgba, bool enabled = true);
  // Call between frames after a change of source, pause, or mode transition.
  void resetHistory();
  const std::string& deviceName() const;
  double networkMs() const;  // model dispatches in the last processed frame (zero in bypass)
  double gpuMs() const;      // upload through readback on the GPU timeline

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
