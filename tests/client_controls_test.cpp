#include "client_controls.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
using namespace client;
static bool close(double a, double b) { return std::abs(a-b) < 1e-5; }
int main() {
  // Catch accepting truncated/negative/overflow CLI dimensions and short benchmarks.
  assert(parseInteger("1024", 64, 16384) == 1024);
  for (auto s : {"-1", "0", "63", "1024x", "9999999999999999999", ""}) {
    bool rejected = false; try { parseInteger(s, 64, 16384); } catch (...) { rejected = true; }
    assert(rejected);
  }
  assert(validBenchmark(120, 0, -1));
  assert(!validBenchmark(119, 30, -1));
  assert(!validBenchmark(120, 30, 149));
  assert(validBenchmark(120, 30, 152));
  // Catch stretching exact square output into a constrained rectangular window.
  auto viewport = letterbox(1024,1024,1280,720);
  assert(viewport.left == 280 && viewport.bottom == 0 && viewport.width == 720 && viewport.height == 720);
  viewport = letterbox(1280,720,512,512);
  assert(viewport.left == 0 && viewport.bottom == 112 && viewport.width == 512 && viewport.height == 288);
  // Catch fitting only vertically: portrait view must still contain the bounding sphere.
  const auto landscape = fitBounds(2, 45, 1);
  const auto portrait = fitBounds(2, 45, 0.5);
  assert(landscape.distance > 5.22 && landscape.distance < 5.23);
  assert(portrait.distance > landscape.distance * 1.8);
  assert(landscape.nearPlane > 0 && landscape.farPlane > landscape.distance + 2);
  // Catch orbit translation, polar singularities and zoom crossing the target.
  auto v = orbitEye({1,2,3}, 0, 0, 5);
  assert(close(v.x,1) && close(v.y,2) && close(v.z,8));
  v = orbitEye({1,2,3}, 1.57079632679, 0, 5);
  assert(close(v.x,6) && close(v.z,3));
  v = orbitEye({0,0,0},0,2,5);
  assert(v.y < -4.998 && v.y > -5);
  assert(close(zoomDistance(5,1,0.01), 4.303539882));
  assert(zoomDistance(5,1000,0.01) >= 0.01);
  v = panOffset(0,0,10,0,2,100);
  assert(close(v.x,-0.4) && close(v.y,0) && close(v.z,0));
  assert(!skinCompatible({256,257}));
  assert(skinCompatible({0,256}));
  // Catch biased even median and P95 off-by-one on warm benchmark summaries.
  std::vector<double> samples; for (int i=1; i<=120; ++i) samples.push_back(i);
  auto stats = summarize(samples);
  assert(close(stats.median,60.5) && close(stats.p95,114));
  // Catch overwriting source metadata while a parity's frame0 query is still unavailable at frame2.
  TimingSlot slot;
  assert(slot.reserve(0,true));
  assert(!slot.consume(false,0));
  assert(!slot.reserve(2,false));
  assert(slot.sourceFrame == 0 && slot.enabled);
  assert(slot.consume(true,100));
  BenchmarkSamples lagged(2,120);
  lagged.addGpu(slot.sourceFrame,true,slot.enabled,10,2);
  assert(lagged.networkMs.empty());
  assert(slot.reserve(4,false));
  assert(!slot.consume(true,100)); // GPU has not executed reset; previous result remains available.
  assert(!slot.reserve(6,true));
  assert(slot.consume(true,200));
  assert(slot.sourceFrame == 4 && !slot.enabled);
  assert(slot.reserve(8,true));
  assert(slot.consume(true,300));
  lagged.addGpu(slot.sourceFrame,true,slot.enabled,10,2);
  assert(lagged.networkMs.size() == 1);
  assert(normalizeAnimationIndex(2,3) == 2);
  assert(normalizeAnimationIndex(2,1) == 0);
  assert(normalizeAnimationIndex(2,0) == 0);
  assert(jsonQuote("a\nb\tc\r\"\\") == "\"a\\nb\\tc\\r\\\"\\\\\"");
  bool fullRejected = false;
  try { writeTextFile("/dev/full","small buffered benchmark result"); } catch (...) { fullRejected = true; }
  assert(fullRejected);
  // Catch black bare-PBR imports, while preserving authored or explicit lighting.
  assert(needsFallbackLighting(0,false,false,false));
  assert(!needsFallbackLighting(1,false,false,false));
  assert(!needsFallbackLighting(0,true,false,false));
  assert(!needsFallbackLighting(0,false,true,false));
  assert(!needsFallbackLighting(0,false,false,true));
  // Catch counting stale/parity-lagged/NR-off GPU timestamps as warm samples.
  BenchmarkSamples benchmark{30,120};
  benchmark.addGpu(29,true,true,10,2);
  benchmark.addGpu(30,false,true,10,2);
  benchmark.addGpu(30,true,false,10,2);
  assert(benchmark.networkMs.empty());
  benchmark.addGpu(30,true,true,10,2);
  benchmark.addGpu(31,true,true,std::numeric_limits<double>::quiet_NaN(),2);
  benchmark.addGpu(31,true,true,10,-1);
  benchmark.addGpu(30,true,true,10,2);
  assert(benchmark.networkMs.size() == 1);
  for (unsigned i=31;i<150;++i) benchmark.addGpu(i,true,true,10,2);
  for (unsigned i=30;i<149;++i) benchmark.addFrame(i,16);
  assert(!benchmark.complete());
  benchmark.addFrame(149,16);
  assert(benchmark.complete());
  std::cout << "client controls: all checks passed\n";
}
