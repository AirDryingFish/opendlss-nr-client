#pragma once

// Opt-in native acceptance check. All interaction enters this process's SDL
// queue; click coordinates come only from observed, visible ImGui items.
#include "asset_catalog.h"
#include "client_controls.h"

#include <SDL.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ImGui { IMGUI_API void ClearIniSettings(); }

struct AssetImportScenario {
  static constexpr Uint32 kMouseId = 0x4e52494d;
  bool done = false;
  std::string error;

  AssetImportScenario(std::string outputJson, const std::filesystem::path& assetsRoot)
      : output_(std::move(outputJson)) {
    if (!active()) return;
    try {
      if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::ClearIniSettings();
      }
      const auto assets = client::discoverAssets(assetsRoot);
      discovered_ = assets.size();
      fox_ = std::filesystem::canonical(assetsRoot / "cases/fox/Model.glb").string();
      require(std::any_of(assets.begin(),assets.end(),[&](const auto& entry) { return entry.model == fox_; }),
          "startup asset discovery did not include the bundled Fox");
      scratch_ = std::filesystem::temp_directory_path() / ("nr asset import with spaces " +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::filesystem::create_directories(scratch_ / "bad environment");
      const auto valid = scratch_ / "Imported Fox.GLB";
      const auto malformed = scratch_ / "malformed.gLb";
      const auto badEnvironment = scratch_ / "bad environment/Model.GLB";
      std::filesystem::copy_file(fox_,valid);
      std::filesystem::copy_file(fox_,badEnvironment);
      client::writeTextFile(malformed.string(),"deliberately malformed GLB");
      client::writeTextFile((badEnvironment.parent_path() / "view.json").string(),
          "{\"name\":\"Invalid environment fixture\",\"model\":\"Model.GLB\",\"environment\":\"missing.hdr\"}");
      valid_ = std::filesystem::canonical(valid).string();
      malformed_ = std::filesystem::canonical(malformed).string();
      badEnvironment_ = std::filesystem::canonical(badEnvironment).string();
    } catch (const std::exception& e) { fail(e.what()); }
  }

  ~AssetImportScenario() {
    if (!scratch_.empty()) { std::error_code ignored; std::filesystem::remove_all(scratch_,ignored); }
  }
  bool active() const { return !output_.empty(); }
  bool timedOut() {
    if (active() && !done && std::chrono::duration<double>(std::chrono::steady_clock::now()-started_).count() > 45)
      fail("asset import scenario exceeded its 45 second deadline");
    return done;
  }

  // Call immediately after each item. Values identify assets/entries; other
  // actions use just their name. Clipped targets scroll within their UI window.
  void observe(const char* action, const std::string& value) {
    if (!active() || done) return;
    const std::string key = itemKey(action,value);
    Item item{ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),ImGui::GetFrameCount(),
        ImGui::IsItemVisible(),ImGui::IsItemHovered(),ImGui::IsItemActive()};
    items_[key] = item;
    if (key == "path" || key == "go") lastBrowserPath_ = value;
    if (key == "path") { lastPathActive_ = item.active; lastPathFrame_ = item.frame; }
    if (!clickPhase_ && key == desiredItem() && (key != scrollTarget_ || !item.visible)) {
      ImGui::SetScrollHereY(0.5f);
      scrollTarget_ = key;
      scrollFrame_ = ImGui::GetFrameCount();
    }
  }

  // Call after UI drawing/endFrame. Startup selection, Browse, and import must
  // remain the normal production UI/queue transactions; this method only emits
  // SDL input and checks their later observable results.
  void frame(SDL_Window* window, unsigned frameIndex, const std::string& currentModel,
      size_t sceneCount, const std::string& importError, bool browserOpen, bool nrOn, double networkMs) {
    if (!active() || done || timedOut()) return;
    try {
      completedFrames_ = frameIndex;
      lastModel_ = currentModel;
      lastBrowserOpen_ = browserOpen;
      traceState(frameIndex);
      require(frameIndex < 600,"asset import scenario exceeded 600 rendered frames");
      require(nrOn,"NR was disabled during the asset import scenario");
      if (!initialized_) {
        require(sceneCount >= discovered_,"startup scene catalog is missing discovered asset entries");
        require(canonical(currentModel) != fox_,"scenario must start on a model other than Fox");
        require(importError.empty(),"startup scene already has an import error");
        initialCount_ = expectedCount_ = sceneCount;
        expectedModel_ = canonical(currentModel);
        beforeError_ = importError;
        initialized_ = true;
        stepFrame_ = frameIndex;
        SDL_RaiseWindow(window);
      }
      if (frameIndex < stepFrame_ + 2) return;
      if (action_ == 0) {
        if (step_ == 0 && click(window,frameIndex,"catalog",{})) nextStep(frameIndex);
        else if (step_ == 1 && click(window,frameIndex,"asset",fox_)) nextStep(frameIndex);
        else if (step_ == 2 && canonical(currentModel) == fox_) {
          require(sceneCount == initialCount_,"catalog selection appended a duplicate scene");
          require(importError.empty(),"selecting startup Fox produced an import error");
          if (settled(frameIndex,networkMs)) {
            expectedModel_ = fox_;
            finishAction(frameIndex,sceneCount,importError);
          }
        }
      } else if (action_ == 1 || action_ == 2) {
        if (step_ == 0 && click(window,frameIndex,"browse",{})) nextStep(frameIndex);
        else if (step_ == 1 && (browserOpen || clickPhase_)) {
          if (action_ == 1) {
            if (click(window,frameIndex,"cancel",{})) nextStep(frameIndex);
          } else {
            key(window,SDL_KEYDOWN,SDLK_ESCAPE,KMOD_NONE);
            nextStep(frameIndex);
          }
        } else if (step_ == 2 && action_ == 2) {
          key(window,SDL_KEYUP,SDLK_ESCAPE,KMOD_NONE);
          nextStep(frameIndex);
        } else if (step_ >= 2 && !browserOpen) {
          unchanged(currentModel,sceneCount);
          require(importError == beforeError_,"cancelled Browse changed import status");
          finishAction(frameIndex,sceneCount,importError);
        }
      } else {
        const std::string& target = fixture();
        if (step_ == 0 && click(window,frameIndex,"browse",{})) nextStep(frameIndex);
        else if (step_ == 1 && browserOpen && click(window,frameIndex,"path",{})) nextStep(frameIndex);
        else if (step_ == 2 && typePath(window,frameIndex,std::filesystem::path(target).parent_path().string())) nextStep(frameIndex);
        else if (step_ == 3 && click(window,frameIndex,"go",{})) nextStep(frameIndex);
        else if (step_ == 4 && click(window,frameIndex,"entry",target)) nextStep(frameIndex);
        else if (step_ == 5 && click(window,frameIndex,"open",{})) nextStep(frameIndex);
        else if (step_ == 6 && !browserOpen) {
          if (action_ == 3) {
            if (canonical(currentModel) != valid_) {
              require(importError.empty(),"valid external Fox import failed: " + importError);
              return;
            }
            require(sceneCount == initialCount_ + 1,"external import did not append exactly one scene");
            require(importError.empty(),"valid external import retained an error");
          } else {
            if (importError.empty() || importError == beforeError_) return;
            unchanged(currentModel,sceneCount);
            if (action_ == 5) require(importError.find("missing.hdr") != std::string::npos,
                "valid GLB with missing HDR did not reach renderer construction failure");
          }
          if (settled(frameIndex,networkMs)) {
            if (action_ == 3) { expectedModel_ = valid_; expectedCount_ = sceneCount; }
            finishAction(frameIndex,sceneCount,importError);
          }
        }
      }
    } catch (const std::exception& e) { fail(e.what()); }
  }

  bool write() {
    if (!active()) return true;
    if (!done && error.empty()) error = "asset import scenario interrupted";
    std::ostringstream result;
    result << "{\n  \"complete\":" << (done && error.empty() ? "true" : "false")
           << ",\n  \"error\":" << client::jsonQuote(error)
           << ",\n  \"scope\":\"Native SDL/ImGui event-driven asset selection/import and transactional rollback; visual layout requires separate inspection\""
           << ",\n  \"completed_frames\":" << completedFrames_ << ",\n  \"actions_passed\":" << action_
           << ",\n  \"discovered_assets\":" << discovered_ << ",\n  \"startup_scene_count\":" << initialCount_
           << ",\n  \"final_scene_count\":" << expectedCount_
           << ",\n  \"fox_model\":" << client::jsonQuote(fox_)
           << ",\n  \"external_model\":" << client::jsonQuote(valid_)
           << ",\n  \"failed_action\":" << action_ << ",\n  \"failed_step\":" << step_
           << ",\n  \"diagnostic\":{\"last_desired\":" << client::jsonQuote(desiredItem())
           << ",\"click_phase\":" << clickPhase_ << ",\"step_frame\":" << stepFrame_
           << ",\"scroll_target\":" << client::jsonQuote(scrollTarget_) << ",\"scroll_frame\":" << scrollFrame_
           << ",\"ui_frame\":" << ImGui::GetFrameCount()
           << ",\"current_model\":" << client::jsonQuote(lastModel_)
           << ",\"browser_open\":" << (lastBrowserOpen_ ? "true" : "false")
           << ",\"last_browser_path\":" << client::jsonQuote(lastBrowserPath_)
           << ",\"path_active\":" << (lastPathActive_ ? "true" : "false") << ",\"path_ui_frame\":" << lastPathFrame_
           << ",\"catalog\":" << itemJson("catalog")
           << ",\"path\":" << itemJson("path") << ",\"go\":" << itemJson("go")
           << ",\"desired_item\":" << itemJson(desiredItem())
           << ",\"last_sent_frame\":" << lastSentFrame_ << ",\"last_sent_type\":" << client::jsonQuote(lastSentType_)
           << ",\"last_sent_mouse_id\":" << lastSentMouseId_
           << ",\"last_sent_coords\":[" << lastSentPosition_.x << ',' << lastSentPosition_.y << "]}"
           << ",\n  \"input_trace_frame_limit\":" << traceLimit()
           << ",\n  \"input_trace\":[\n" << trace_.str() << "\n  ]"
           << ",\n  \"checks\":[\n" << checks_.str() << "\n  ]\n}\n";
    try { client::writeTextFile(output_,result.str()); }
    catch (const std::exception& e) { error = e.what(); return false; }
    return done && error.empty();
  }

 private:
  struct Item { ImVec2 min{}, max{}; int frame = 0; bool visible = false, hovered = false, active = false; };
  static void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
  static std::string canonical(const std::string& path) {
    std::error_code error;
    const auto resolved = std::filesystem::canonical(path,error);
    return error ? path : resolved.string();
  }
  static std::string itemKey(const char* action, const std::string& value) {
    const std::string name(action);
    return name + ((name == "asset" || name == "entry") ? "\n" + value : "");
  }
  const std::string& fixture() const { return action_ == 3 ? valid_ : action_ == 4 ? malformed_ : badEnvironment_; }
  std::string desiredItem() const {
    if (action_ == 0) return itemKey(step_ == 0 ? "catalog" : "asset",fox_);
    if (step_ == 0) return "browse";
    if (action_ == 1) return "cancel";
    if (action_ == 2) return {};
    if (step_ == 1) return "path";
    if (step_ == 3) return "go";
    if (step_ == 4) return itemKey("entry",fixture());
    if (step_ == 5) return "open";
    return {};
  }
  void fail(const std::string& message) {
    error = message + " (action " + std::to_string(action_) + ", step " + std::to_string(step_) + ")";
    done = true;
  }
  std::string itemJson(const std::string& key) const {
    const auto found = items_.find(key);
    if (found == items_.end()) return "null";
    const auto& item = found->second;
    std::ostringstream text;
    text << "{\"min\":[" << item.min.x << ',' << item.min.y << "],\"max\":[" << item.max.x << ',' << item.max.y
         << "],\"observed_ui_frame\":" << item.frame << ",\"fresh\":"
         << (item.frame == ImGui::GetFrameCount() ? "true" : "false")
         << ",\"visible\":" << (item.visible ? "true" : "false")
         << ",\"hovered\":" << (item.hovered ? "true" : "false")
         << ",\"active\":" << (item.active ? "true" : "false") << '}';
    return text.str();
  }
  void appendTrace(const std::string& sample) {
    if (traceCount_++) trace_ << ",\n";
    trace_ << "    " << sample;
    fprintf(stderr,"[asset-import-test] %s\n",sample.c_str());
  }
  static unsigned traceLimit() { return std::getenv("DLSS5_DEMO_IMPORT_TRACE") ? 600u : 80u; }
  void traceState(unsigned frame) {
    if (frame > traceLimit()) return;
    const auto& io = ImGui::GetIO();
    std::ostringstream text;
    text << "{\"kind\":\"frame\",\"frame\":" << frame << ",\"ui_frame\":" << ImGui::GetFrameCount()
         << ",\"action\":" << action_ << ",\"step\":" << step_ << ",\"click_phase\":" << clickPhase_
         << ",\"desired\":" << client::jsonQuote(desiredItem()) << ",\"item\":" << itemJson(desiredItem())
         << ",\"catalog\":" << itemJson("catalog")
         << ",\"path\":" << itemJson("path") << ",\"go\":" << itemJson("go")
         << ",\"last_browser_path\":" << client::jsonQuote(lastBrowserPath_)
         << ",\"path_active\":" << (lastPathActive_ ? "true" : "false") << ",\"path_ui_frame\":" << lastPathFrame_
         << ",\"mouse\":[" << io.MousePos.x << ',' << io.MousePos.y << "],\"mouse_down\":"
         << (io.MouseDown[0] ? "true" : "false") << ",\"want_capture_mouse\":"
         << (io.WantCaptureMouse ? "true" : "false") << ",\"browser_open\":"
         << (lastBrowserOpen_ ? "true" : "false") << '}';
    appendTrace(text.str());
  }
  void traceClick(unsigned frame, const char* action, const std::string& value, const SDL_Event& event) {
    lastSentFrame_ = frame;
    lastSentType_ = event.type == SDL_MOUSEMOTION ? "motion" : event.type == SDL_MOUSEBUTTONDOWN ? "down" : "up";
    lastSentPosition_ = event.type == SDL_MOUSEMOTION ? ImVec2(float(event.motion.x),float(event.motion.y)) :
        ImVec2(float(event.button.x),float(event.button.y));
    lastSentMouseId_ = event.type == SDL_MOUSEMOTION ? event.motion.which : event.button.which;
    if (frame > traceLimit()) return;
    const auto& io = ImGui::GetIO();
    std::ostringstream text;
    text << "{\"kind\":\"sent_click\",\"frame\":" << frame << ",\"ui_frame\":" << ImGui::GetFrameCount()
         << ",\"action\":" << action_ << ",\"step\":" << step_ << ",\"click_phase\":" << clickPhase_
         << ",\"target\":" << client::jsonQuote(itemKey(action,value)) << ",\"item\":" << itemJson(itemKey(action,value))
         << ",\"type\":" << client::jsonQuote(lastSentType_) << ",\"sent_coords\":["
         << lastSentPosition_.x << ',' << lastSentPosition_.y << "],\"mouse\":[" << io.MousePos.x << ',' << io.MousePos.y
         << "],\"mouse_down\":" << (io.MouseDown[0] ? "true" : "false") << ",\"which\":" << lastSentMouseId_
         << ",\"last_browser_path\":" << client::jsonQuote(lastBrowserPath_)
         << ",\"path_active\":" << (lastPathActive_ ? "true" : "false") << '}';
    appendTrace(text.str());
  }
  static void send(SDL_Event& event) {
    if (SDL_PushEvent(&event) != 1) throw std::runtime_error("SDL_PushEvent failed");
  }
  static void key(SDL_Window* window, Uint32 type, SDL_Keycode symbol, SDL_Keymod modifiers) {
    SDL_Event event{};
    event.type = type;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.sym = symbol;
    event.key.keysym.scancode = SDL_GetScancodeFromKey(symbol);
    event.key.keysym.mod = Uint16(modifiers);
    send(event);
  }
  bool click(SDL_Window* window, unsigned frame, const char* action, const std::string& value) {
    if (clickPhase_ == 3) { clickPhase_ = 0; return true; }
    if (!clickPhase_) {
      const auto found = items_.find(itemKey(action,value));
      if (found == items_.end() || found->second.frame != ImGui::GetFrameCount() || !found->second.visible) return false;
      if (ImGui::GetFrameCount() < scrollFrame_ + 2) return false;
      const auto& item = found->second;
      if (item.max.x <= item.min.x || item.max.y <= item.min.y) return false;
      clickPosition_ = ImVec2((item.min.x+item.max.x)*0.5f,(item.min.y+item.max.y)*0.5f);
      const auto display = ImGui::GetIO().DisplaySize;
      if (clickPosition_.x < 0 || clickPosition_.y < 0 || clickPosition_.x >= display.x || clickPosition_.y >= display.y) return false;
    }
    SDL_Event event{};
    event.type = !clickPhase_ ? SDL_MOUSEMOTION : clickPhase_ == 1 ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    if (!clickPhase_) {
      event.motion.windowID = SDL_GetWindowID(window);
      event.motion.which = kMouseId;
      event.motion.x = int(clickPosition_.x); event.motion.y = int(clickPosition_.y);
    } else {
      event.button.windowID = SDL_GetWindowID(window);
      event.button.which = kMouseId;
      event.button.button = SDL_BUTTON_LEFT;
      event.button.state = clickPhase_ == 1 ? SDL_PRESSED : SDL_RELEASED;
      event.button.x = int(clickPosition_.x); event.button.y = int(clickPosition_.y);
    }
    send(event);
    traceClick(frame,action,value,event);
    ++clickPhase_;
    stepFrame_ = frame;
    return false;
  }
  bool typePath(SDL_Window* window, unsigned frame, const std::string& text) {
    if (textPhase_ == 0) key(window,SDL_KEYDOWN,SDLK_a,SDL_Keymod(KMOD_CTRL));
    else if (textPhase_ == 1) {
      key(window,SDL_KEYUP,SDLK_a,SDL_Keymod(KMOD_CTRL));
      key(window,SDL_KEYUP,SDLK_LCTRL,KMOD_NONE);
    } else if (textPhase_ == 2) {
      for (size_t begin=0;begin<text.size();) {
        size_t end = std::min(begin+sizeof(SDL_TextInputEvent{}.text)-1,text.size());
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        require(end > begin,"cannot encode fixture directory as SDL text input");
        SDL_Event event{};
        event.type = SDL_TEXTINPUT;
        event.text.windowID = SDL_GetWindowID(window);
        std::memcpy(event.text.text,text.data()+begin,end-begin);
        send(event);
        begin = end;
      }
    } else { textPhase_ = 0; return true; }
    ++textPhase_;
    stepFrame_ = frame;
    return false;
  }
  void nextStep(unsigned frame) { ++step_; stepFrame_ = frame; clickPhase_ = textPhase_ = 0; }
  void unchanged(const std::string& model, size_t count) const {
    require(canonical(model) == expectedModel_,"cancelled/failed import replaced the current model");
    require(count == expectedCount_,"cancelled/failed import changed the scene catalog");
  }
  bool settled(unsigned frame, double networkMs) {
    if (!settleFrame_) settleFrame_ = frame;
    if (std::isfinite(networkMs) && networkMs > 0) { networkTotal_ += networkMs; ++networkSamples_; }
    if (frame < settleFrame_ + 12) return false;
    require(networkSamples_ > 0,"no positive NR GPU timing after asset selection/import");
    return true;
  }
  void finishAction(unsigned frame, size_t count, const std::string& importError) {
    const char* names[] = {"startup_catalog_fox","browse_cancel","browse_escape","external_fox_import",
        "malformed_glb_rollback","missing_hdr_renderer_rollback"};
    if (action_) checks_ << ",\n";
    checks_ << "    {\"action\":" << client::jsonQuote(names[action_]) << ",\"frame\":" << frame
            << ",\"scene_count\":" << count << ",\"passed\":true,\"network_samples\":" << networkSamples_;
    if (networkSamples_) checks_ << ",\"network_gpu_ms\":" << networkTotal_/networkSamples_;
    checks_ << ",\"import_error\":" << client::jsonQuote(importError) << '}';
    ++action_;
    step_ = clickPhase_ = textPhase_ = settleFrame_ = networkSamples_ = 0;
    networkTotal_ = 0;
    stepFrame_ = frame;
    beforeError_ = importError;
    if (action_ == 6) done = true;
  }
  std::string output_, fox_, valid_, malformed_, badEnvironment_, expectedModel_, beforeError_, scrollTarget_;
  std::string lastModel_, lastSentType_, lastBrowserPath_;
  std::filesystem::path scratch_;
  std::map<std::string,Item> items_;
  std::ostringstream checks_, trace_;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  size_t discovered_ = 0, initialCount_ = 0, expectedCount_ = 0;
  unsigned action_ = 0, step_ = 0, stepFrame_ = 0, clickPhase_ = 0, textPhase_ = 0;
  unsigned settleFrame_ = 0, networkSamples_ = 0, completedFrames_ = 0;
  ImVec2 clickPosition_{};
  ImVec2 lastSentPosition_{};
  int scrollFrame_ = 0;
  int lastPathFrame_ = 0;
  unsigned traceCount_ = 0, lastSentFrame_ = 0;
  Uint32 lastSentMouseId_ = 0;
  double networkTotal_ = 0;
  bool initialized_ = false, lastBrowserOpen_ = false, lastPathActive_ = false;
};
