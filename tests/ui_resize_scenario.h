#pragma once

// Opt-in diagnostic: inject only into this process's SDL queue. Coordinates come
// from actual ImGui items; success proves logical hit testing, not visual alignment.
struct UiResizeScenario {
  struct Item {
    ImVec2 min{}, max{};
    bool hovered = false;
    void observe() { min = ImGui::GetItemRectMin(); max = ImGui::GetItemRectMax(); hovered = ImGui::IsItemHovered(); }
  } apply, enabled;
  std::string output, error;
  std::ostringstream samples;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  unsigned action = 0, phase = 0, phaseFrame = 0, completed = 0, skipped = 0, resizeEvents = 0;
  bool done = false, firstSample = true, geometryMatches = true, windowPreserved = true;
  int initialW = 0, initialH = 0;
  // Apply exact 2048, 512, 2048, then repeat the same size. Toggle off/on after each.
  const int sizes[4] = {2048, 512, 2048, 2048};
  explicit UiResizeScenario(std::string path) : output(std::move(path)) {}
  bool active() const { return !output.empty(); }
  bool timedOut() {
    if (active() && !done && std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() > 25) {
      error = "25 second UI scenario deadline exceeded"; done = true;
    }
    return done;
  }
  void push(SDL_Window* window, Uint32 type, ImVec2 position) {
    SDL_Event event{};
    event.type = type;
    if (type == SDL_MOUSEMOTION) {
      event.motion.windowID = SDL_GetWindowID(window);
      event.motion.x = int(position.x); event.motion.y = int(position.y);
    } else {
      event.button.windowID = SDL_GetWindowID(window);
      event.button.button = SDL_BUTTON_LEFT;
      event.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
      event.button.x = int(position.x); event.button.y = int(position.y);
    }
    if (SDL_PushEvent(&event) != 1) { error = "SDL_PushEvent failed"; done = true; }
  }
  void frame(SDL_Window* window, unsigned frameIndex, unsigned width, unsigned height,
             int& customWidth, int& customHeight, bool isEnabled, const filament::Viewport& viewport) {
    if (!active() || done) return;
    ++completed;
    const auto& io = ImGui::GetIO();
    int w = 0, h = 0; SDL_GetWindowSize(window, &w, &h);
    int nativeW = -1, nativeH = -1;
#if defined(FILAMENT_SUPPORTS_X11)
    SDL_SysWMinfo info{}; SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window,&info) && info.subsystem == SDL_SYSWM_X11) {
      XWindowAttributes attributes{};
      if (XGetWindowAttributes(info.info.x11.display,info.info.x11.window,&attributes)) {
        nativeW = attributes.width; nativeH = attributes.height;
      }
    }
#endif
    if (!firstSample) samples << ",\n";
    firstSample = false;
    auto vec = [&](ImVec2 v) { samples << '[' << v.x << ',' << v.y << ']'; };
    samples << "    {\"frame\":" << frameIndex << ",\"ui_frame\":" << ImGui::GetFrameCount()
            << ",\"action\":" << action << ",\"phase\":" << phase << ",\"nr_enabled\":" << (isEnabled ? "true" : "false")
            << ",\"native_window\":[" << nativeW << ',' << nativeH << ']'
            << ",\"render\":[" << width << ',' << height << "],\"sdl_window\":[" << w << ',' << h << "],\"display\":";
    vec(io.DisplaySize); samples << ",\"framebuffer_scale\":"; vec(io.DisplayFramebufferScale);
    samples << ",\"ui_viewport\":[" << viewport.left << ',' << viewport.bottom << ',' << viewport.width << ',' << viewport.height
            << "],\"mouse\":"; vec(io.MousePos);
    samples << ",\"mouse_down\":" << (io.MouseDown[0] ? "true" : "false")
            << ",\"want_capture_mouse\":" << (io.WantCaptureMouse ? "true" : "false")
            << ",\"window_hovered\":" << (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ? "true" : "false")
            << ",\"apply_min\":"; vec(apply.min); samples << ",\"apply_max\":"; vec(apply.max);
    samples << ",\"enabled_min\":"; vec(enabled.min); samples << ",\"enabled_max\":"; vec(enabled.max);
    samples << ",\"apply_hovered\":" << (apply.hovered ? "true" : "false")
            << ",\"enabled_hovered\":" << (enabled.hovered ? "true" : "false")
            << ",\"begin_frame_skips\":" << skipped << ",\"sdl_resize_events\":" << resizeEvents << '}';
    if (completed == 8) { initialW = w; initialH = h; }
    if (completed < 8 || frameIndex < phaseFrame + (phase == 3 ? 5u : 1u)) return;
    const bool isApply = action % 3 == 0;
    const Item& item = isApply ? apply : enabled;
    const ImVec2 center{(item.min.x+item.max.x)*0.5f, (item.min.y+item.max.y)*0.5f};
    if (phase == 0) {
      if (isApply) customWidth = customHeight = sizes[action/3];
      push(window, SDL_MOUSEMOTION, center);
    } else if (phase == 1) push(window, SDL_MOUSEBUTTONDOWN, center);
    else if (phase == 2) push(window, SDL_MOUSEBUTTONUP, center);
    else {
      windowPreserved = windowPreserved && w == initialW && h == initialH;
      geometryMatches = geometryMatches && io.DisplaySize.x == w && io.DisplaySize.y == h &&
          viewport.width == unsigned(w) && viewport.height == unsigned(h) &&
          (nativeW < 0 || (nativeW == w && nativeH == h));
      const bool correct = isApply ? (width == unsigned(sizes[action/3]) && height == width) : (isEnabled == (action % 3 == 2));
      fprintf(stderr, "[ui-resize-test] action %u (%s) %s at frame %u, window %dx%d, display %.0fx%.0f\n",
              action, isApply ? "Apply" : "Enabled", correct ? "PASS" : "FAIL", frameIndex, w, h, io.DisplaySize.x, io.DisplaySize.y);
      if (!correct) { error = "SDL click did not produce expected state at action " + std::to_string(action); done = true; return; }
      if (++action == 12) {
        if (!windowPreserved) error = "offscreen Apply changed SDL window dimensions";
        else if (!geometryMatches) error = "settled native window/ImGui/viewport dimensions differ";
        done = true; return;
      }
    }
    phase = (phase + 1) % 4; phaseFrame = frameIndex;
  }
  bool write() {
    if (!active()) return true;
    if (!done && error.empty()) error = "scenario interrupted";
    std::ostringstream result;
    result << "{\n  \"complete\": " << (done && error.empty() ? "true" : "false")
           << ",\n  \"scope\": \"logical SDL/ImGui hit testing; rendered alignment requires visual verification\",\n  \"error\": " << client::jsonQuote(error)
           << ",\n  \"logical_clicks_passed\": " << action
           << ",\n  \"window_preserved\": " << (windowPreserved ? "true" : "false")
           << ",\n  \"geometry_matches\": " << (geometryMatches ? "true" : "false") << ",\n  \"completed_frames\": " << completed
           << ",\n  \"begin_frame_skips\": " << skipped << ",\n  \"samples\": [\n" << samples.str() << "\n  ]\n}\n";
    try { client::writeTextFile(output,result.str()); }
    catch (const std::exception& e) { fprintf(stderr,"[ui-resize-test] %s\n",e.what()); return false; }
    return done && error.empty();
  }
};
