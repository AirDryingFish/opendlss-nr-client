#pragma once

// Opt-in real-renderer check: a broken replacement, lost look setting, or
// destructive failed import must fail this scenario. Call beforeFrame before
// beginFrame, and afterFrame after endFrame with the rendered frame count.
#include "environment_presets.h"
#include "nr_pass.h"
#include "scene.h"

#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/Scene.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct EnvironmentScenario {
  bool done = false;
  std::string error;

  EnvironmentScenario(std::string prefix, const std::vector<client::EnvironmentPreset>& presets)
      : prefix_(std::move(prefix)), presets_(presets) {
    if (active() && presets_.size() < 4) {
      error = "environment scenario requires four presets";
      done = true;
    }
  }
  ~EnvironmentScenario() {
    if (!scratch_.empty()) { std::error_code ignored; std::filesystem::remove_all(scratch_,ignored); }
  }
  bool active() const { return !prefix_.empty(); }

  void beforeFrame(unsigned frameIndex, LoadedScene& loaded, filament::Engine& engine,
      float& intensity, float& rotation, bool& background, bool& historyReset, int& activePreset) {
    if (!active() || done) return;
    try {
      require(frameIndex < 1000,"environment scenario exceeded 1000 rendered frames");
      require(std::chrono::duration<double>(std::chrono::steady_clock::now()-started_).count() < 45,
          "environment scenario exceeded its 45 second deadline");
      if (armed_) return;
      if (stage_ == 0) {
        const auto parent = std::filesystem::path(prefix_).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        intensity = 0.73f;
        rotation = 0.4f;
        background = false;
        loaded.setEnvironmentIntensity(intensity);
        loaded.setEnvironmentRotation(rotation);
        loaded.setBackgroundEnvironment(background);
      }
      if (stage_ == 4) rollbackChecks(loaded);
      // Four visible-environment captures, then A -> B -> A with solid
      // backgrounds on A and B, and the environment background restored on A.
      if (stage_ >= 4) {
        background = stage_ == 6;
        loaded.setBackgroundEnvironment(background);
      }
      const size_t selected = stage_ < 4 ? stage_ : stage_ == 5 ? 1 : 0;
      const auto before = snapshot(loaded);
      loaded.setEnvironment(presets_[selected].path);
      const auto after = snapshot(loaded);
      require(after.path == presets_[selected].path,"successful replacement did not select the requested HDR path");
      require(after.light && after.light != before.light,"successful replacement did not replace the environment light");
      require(after.light->getReflectionsTexture(),"successful replacement has no reflection texture");
      require(after.background == before.background,"successful replacement changed background visibility");
      require(sameRotation(before.matrix,after.matrix),"successful replacement changed the light rotation");
      require(before.background ? after.skybox != before.skybox : after.skybox == before.skybox,
          "successful replacement did not preserve the chosen background mode");
      checkLook(loaded,background);
      historyReset = true;
      activePreset = int(selected);
      ++successes_;
      if (stage_ >= 4) ++roundTrips_;
      if (!firstCheck_) checks_ << ",\n";
      firstCheck_ = false;
      checks_ << "    {\"stage\":" << stage_ << ",\"frame\":" << frameIndex
              << ",\"id\":" << client::jsonQuote(presets_[selected].id)
              << ",\"path_changed\":" << (before.path != after.path ? "true" : "false")
              << ",\"light_replaced\":true,\"look_preserved\":true,\"history_reset_requested\":true"
              << ",\"background_environment\":" << (background ? "true" : "false") << '}';
      if (stage_ == 0) {
        background = true;
        loaded.setBackgroundEnvironment(true);
      }
      engine.flushAndWait();
      startFrame_ = frameIndex;
      networkSamples_ = 0;
      networkTotal_ = 0;
      armed_ = true;
    } catch (const std::exception& e) { error = e.what(); done = true; }
  }

  void afterFrame(unsigned frameIndex, NrPass& nr, filament::Engine& engine) {
    if (!active() || done || !armed_) return;
    try {
      completedFrames_ = frameIndex;
      chainTimeout_ = nr.chainTimedOut();
      require(!chainTimeout_,"NR chained execution timed out during the environment scenario");
      if (nr.timingsFresh() && nr.timingsEnabled() && nr.timingsFrame() >= startFrame_ &&
          std::isfinite(nr.timings().networkMs) && nr.timings().networkMs > 0) {
        networkTotal_ += nr.timings().networkMs;
        ++networkSamples_;
      }
      if (frameIndex < startFrame_ + 30) return;
      if (stage_ < 4) {
        require(networkSamples_ > 0,"no fresh enabled NR network timing was recorded for the HDR preset");
        engine.flushAndWait();
        const std::string image = prefix_ + '-' + presets_[stage_].id + ".ppm";
        const std::string scene = prefix_ + '-' + presets_[stage_].id + "-scene.ppm";
        nr.saveOutput(image,false);
        nr.saveOutput(scene,true);
        require(std::filesystem::file_size(image) > 32 && std::filesystem::file_size(scene) > 32,
            "environment capture is empty");
        if (stage_) cases_ << ",\n";
        cases_ << "    {\"id\":" << client::jsonQuote(presets_[stage_].id)
               << ",\"name\":" << client::jsonQuote(presets_[stage_].name)
               << ",\"environment\":" << client::jsonQuote(presets_[stage_].path)
               << ",\"frame\":" << frameIndex << ",\"settled_frames\":" << frameIndex-startFrame_
               << ",\"output\":" << client::jsonQuote(image) << ",\"scene\":" << client::jsonQuote(scene)
               << ",\"network_gpu_ms\":" << networkTotal_/networkSamples_
               << ",\"network_samples\":" << networkSamples_ << '}';
        ++captures_;
      }
      armed_ = false;
      if (++stage_ == 7) done = true;
    } catch (const std::exception& e) { error = e.what(); done = true; }
  }

  bool write() {
    if (!active()) return true;
    if (!done && error.empty()) error = "environment scenario interrupted";
    std::ostringstream result;
    result << "{\n  \"complete\":" << (done && error.empty() ? "true" : "false")
           << ",\n  \"error\":" << client::jsonQuote(error)
           << ",\n  \"intensity\":0.73,\n  \"rotation\":0.4"
           << ",\n  \"completed_frames\":" << completedFrames_
           << ",\n  \"successful_replacements\":" << successes_
           << ",\n  \"history_resets_requested\":" << successes_
           << ",\n  \"rollback_checks_passed\":" << rollbacks_
           << ",\n  \"a_b_a_replacements\":" << roundTrips_
           << ",\n  \"chain_timeout_detected\":" << (chainTimeout_ ? "true" : "false")
           << ",\n  \"captures\":" << captures_
           << ",\n  \"cases\":[\n" << cases_.str() << "\n  ],\n  \"checks\":[\n" << checks_.str() << "\n  ]\n}\n";
    try { client::writeTextFile(prefix_ + ".json",result.str()); }
    catch (const std::exception& e) { error = e.what(); return false; }
    return done && error.empty();
  }

 private:
  struct Snapshot {
    filament::Scene* scene;
    filament::IndirectLight* light;
    filament::Skybox* skybox;
    std::string path;
    float intensity, rotation;
    bool background;
    std::array<float,9> matrix{};
  };
  static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
  }
  static bool close(float a, float b) { return std::isfinite(a) && std::abs(a-b) < 1e-5f; }
  static bool sameRotation(const std::array<float,9>& a, const std::array<float,9>& b) {
    for (size_t i = 0; i < a.size(); ++i) if (!close(a[i],b[i])) return false;
    return true;
  }
  static Snapshot snapshot(LoadedScene& loaded) {
    Snapshot value{loaded.scene(),loaded.scene()->getIndirectLight(),loaded.scene()->getSkybox(),
        loaded.view().environment,loaded.view().environmentIntensity,loaded.view().environmentRotation,
        loaded.view().backgroundEnvironment,{}};
    if (value.light) for (size_t c = 0; c < 3; ++c) for (size_t r = 0; r < 3; ++r)
      value.matrix[c*3+r] = value.light->getRotation()[c][r];
    return value;
  }
  static void checkLook(LoadedScene& loaded, bool background) {
    const auto current = snapshot(loaded);
    require(current.light && close(current.light->getIntensity(),0.73f) && close(current.intensity,0.73f),
        "environment intensity was not preserved");
    require(close(current.rotation,0.4f),"environment rotation setting was not preserved");
    require(current.background == background && current.skybox,"environment background state was not preserved");
  }
  void rollbackChecks(LoadedScene& loaded) {
    scratch_ = std::filesystem::temp_directory_path() / ("nr-environment-scenario-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(scratch_),"cannot create HDR failure fixtures");
    const auto corrupt = scratch_ / "corrupt.hdr";
    client::writeTextFile(corrupt.string(),"deliberately invalid HDR image");
    for (const auto& path : {scratch_ / "missing.hdr",corrupt}) {
      const auto before = snapshot(loaded);
      bool rejected = false;
      try { loaded.setEnvironment(path.string()); }
      catch (const std::exception& e) { rejected = std::string(e.what()).find(path.string()) != std::string::npos; }
      require(rejected,"missing or corrupt HDR replacement did not report its failing path");
      const auto after = snapshot(loaded);
      require(before.scene == after.scene && before.light == after.light && before.skybox == after.skybox &&
          before.path == after.path && close(before.intensity,after.intensity) && close(before.rotation,after.rotation) &&
          before.background == after.background && sameRotation(before.matrix,after.matrix),
          "failed HDR replacement changed the current scene, resources, path, or look");
      checkLook(loaded,before.background);
      ++rollbacks_;
    }
  }
  std::string prefix_;
  std::vector<client::EnvironmentPreset> presets_;
  std::filesystem::path scratch_;
  std::ostringstream cases_, checks_;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  unsigned stage_ = 0, startFrame_ = 0, completedFrames_ = 0;
  unsigned successes_ = 0, captures_ = 0, rollbacks_ = 0, roundTrips_ = 0, networkSamples_ = 0;
  double networkTotal_ = 0;
  bool armed_ = false, firstCheck_ = true, chainTimeout_ = false;
};
