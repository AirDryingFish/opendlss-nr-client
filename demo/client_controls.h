// Dependency-free input/camera math shared by the SDL viewer and native tests.
#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
namespace client {
void validateSceneFile(const std::string& path);
struct Vec3 { double x,y,z; };
struct Framing { double distance, nearPlane, farPlane; };
struct Viewport { int left, bottom; unsigned width, height; };
inline Viewport letterbox(unsigned renderWidth, unsigned renderHeight, unsigned windowWidth, unsigned windowHeight) {
  if (!renderWidth || !renderHeight || !windowWidth || !windowHeight) return {};
  const double scale = std::min(double(windowWidth)/renderWidth,double(windowHeight)/renderHeight);
  const unsigned width = std::max(1u,unsigned(std::floor(renderWidth*scale)));
  const unsigned height = std::max(1u,unsigned(std::floor(renderHeight*scale)));
  return {int((windowWidth-width)/2),int((windowHeight-height)/2),width,height};
}
struct Statistics { double median, p95; };
inline int parseInteger(const std::string& text, int minimum, int maximum) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument("expected an integer in " + std::to_string(minimum) + ".." + std::to_string(maximum));
  size_t end = 0;
  const auto value = std::stoll(text, &end);
  if (end != text.size() || value < minimum || value > maximum)
    throw std::out_of_range("expected an integer in " + std::to_string(minimum) + ".." + std::to_string(maximum));
  return static_cast<int>(value);
}
inline bool validBenchmark(int frames, int warmup, int frameLimit) {
  return frames >= 120 && warmup >= 0 && (frameLimit <= 0 || frameLimit >= frames + warmup + 2);
}
inline Framing fitBounds(double radius, double fovDegrees, double aspect) {
  radius = std::max(radius, 0.001);
  const double vertical = fovDegrees * 3.141592653589793 / 360.0;
  const double limiting = std::min(vertical, std::atan(std::tan(vertical) * std::max(aspect,0.01)));
  const double distance = radius / std::sin(limiting);
  return {distance, std::max(radius * 0.005,0.00001), distance + radius * 50};
}
inline Vec3 orbitEye(Vec3 target, double yaw, double pitch, double distance) {
  pitch = std::clamp(pitch, -1.55, 1.55);
  return {target.x + std::sin(yaw)*std::cos(pitch)*distance,
          target.y - std::sin(pitch)*distance,
          target.z + std::cos(yaw)*std::cos(pitch)*distance};
}
inline double zoomDistance(double distance, double wheelSteps, double minimum) {
  return std::clamp(distance * std::exp(-wheelSteps * 0.15), minimum, minimum * 1000000.0);
}
inline Vec3 panOffset(double yaw, double pitch, double dx, double dy, double distance, double height) {
  const double scale = 2 * distance / std::max(height,1.0);
  return {scale*(-dx*std::cos(yaw) + dy*std::sin(yaw)*std::sin(pitch)),
          scale*dy*std::cos(pitch),
          scale*(dx*std::sin(yaw) + dy*std::cos(yaw)*std::sin(pitch))};
}
inline bool skinCompatible(const std::vector<size_t>& counts) {
  return std::all_of(counts.begin(),counts.end(),[](size_t n){ return n <= 256; });
}
inline Statistics summarize(std::vector<double> values) {
  if (values.empty()) throw std::invalid_argument("no benchmark samples");
  std::sort(values.begin(), values.end());
  const auto n = values.size();
  return {(values[(n-1)/2]+values[n/2])*0.5, values[static_cast<size_t>(std::ceil(n*0.95))-1]};
}
struct BenchmarkSamples {
  int warmup, target;
  BenchmarkSamples(int warmupFrames, int sampleFrames) : warmup(warmupFrames), target(sampleFrames) {}
  std::vector<double> frameMs, gpuFrameMs, networkMs;
  unsigned lastGpuFrame = 0;
  bool hasGpuFrame = false;
  void addFrame(unsigned frame, double ms) {
    if (frame >= static_cast<unsigned>(warmup) && frameMs.size() < static_cast<size_t>(target) && std::isfinite(ms) && ms > 0)
      frameMs.push_back(ms);
  }
  void addGpu(unsigned frame, bool fresh, bool enabled, double frameTime, double networkTime) {
    if (!fresh || !enabled || frame < static_cast<unsigned>(warmup) || (hasGpuFrame && frame <= lastGpuFrame) ||
        networkMs.size() >= static_cast<size_t>(target) || !std::isfinite(frameTime) || !std::isfinite(networkTime) ||
        frameTime <= 0 || networkTime <= 0) return;
    hasGpuFrame = true; lastGpuFrame = frame;
    gpuFrameMs.push_back(frameTime); networkMs.push_back(networkTime);
  }
  bool complete() const {
    return frameMs.size() >= static_cast<size_t>(target) && networkMs.size() >= static_cast<size_t>(target);
  }
};

}
