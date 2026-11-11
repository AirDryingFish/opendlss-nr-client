#pragma once

// Opt-in diagnostic: inject only into this process's SDL queue. Coordinates come
// from actual ImGui items; success proves logical hit testing, not visual alignment.
struct UiResizeScenario {
  struct Item {
    ImVec2 min{}, max{};
    bool hovered = false;
    void observe() { min = ImGui::GetItemRectMin(); max = ImGui::GetItemRectMax(); hovered = ImGui::IsItemHovered(); }
  } enabled;
  std::string output, error;
  std::ostringstream samples;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  unsigned action = 0, phase = 0, phaseFrame = 0, completed = 0, skipped = 0, resizeEvents = 0;
  unsigned resizeRequests = 0, resizesPassed = 0, clicksPassed = 0, actualExtentChanges = 0;
  unsigned geometryStableSince = 0;
  int lastSeenW = 0, lastSeenH = 0, lastCheckedW = 0, lastCheckedH = 0;
  bool done = false, firstSample = true, geometryMatches = true, renderFollowsWindow = true;
  // Landscape, portrait, a potentially WM-clamped request, then repeated dimensions.
  // Requested sizes are stimuli; assertions use the actual settled window extent.
  const int sizes[6][2] = {{800,600}, {1200,900}, {720,960}, {2048,2048}, {1024,768}, {1024,768}};
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
             bool isEnabled, const filament::Viewport& viewport) {
    if (!active() || done) return;
    ++completed;
    const auto& io = ImGui::GetIO();
    int w = 0, h = 0; SDL_GetWindowSize(window, &w, &h);
    if (w != lastSeenW || h != lastSeenH) {
      lastSeenW = w; lastSeenH = h; geometryStableSince = frameIndex;
    }
    if (completed == 8) { lastCheckedW = w; lastCheckedH = h; }
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
            << ",\"requested_window\":[" << sizes[action/3][0] << ',' << sizes[action/3][1] << ']'
            << ",\"native_window\":[" << nativeW << ',' << nativeH << ']'
            << ",\"render\":[" << width << ',' << height << "],\"sdl_window\":[" << w << ',' << h << "],\"display\":";
    vec(io.DisplaySize); samples << ",\"framebuffer_scale\":"; vec(io.DisplayFramebufferScale);
    samples << ",\"ui_viewport\":[" << viewport.left << ',' << viewport.bottom << ',' << viewport.width << ',' << viewport.height
            << "],\"mouse\":"; vec(io.MousePos);
    samples << ",\"mouse_down\":" << (io.MouseDown[0] ? "true" : "false")
            << ",\"want_capture_mouse\":" << (io.WantCaptureMouse ? "true" : "false")
            << ",\"window_hovered\":" << (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ? "true" : "false")
            << ",\"enabled_min\":"; vec(enabled.min); samples << ",\"enabled_max\":"; vec(enabled.max);
    samples << ",\"enabled_hovered\":" << (enabled.hovered ? "true" : "false")
            << ",\"begin_frame_skips\":" << skipped << ",\"sdl_resize_events\":" << resizeEvents << '}';
    if (completed < 8 || frameIndex < phaseFrame + (phase == 3 ? 5u : 1u)) return;
    const bool isResize = action % 3 == 0;
    const int targetW = sizes[action/3][0], targetH = sizes[action/3][1];
    const ImVec2 center{(enabled.min.x+enabled.max.x)*0.5f, (enabled.min.y+enabled.max.y)*0.5f};
    if (isResize && phase == 0) {
      SDL_SetWindowSize(window,targetW,targetH);
      ++resizeRequests;
      phase = 3; phaseFrame = frameIndex; geometryStableSince = frameIndex;
      return;
    }
    if (phase == 0) push(window, SDL_MOUSEMOTION, center);
    else if (phase == 1) push(window, SDL_MOUSEBUTTONDOWN, center);
    else if (phase == 2) push(window, SDL_MOUSEBUTTONUP, center);
    else {
      // Allow asynchronous WM configure notifications to settle before asserting.
      if (isResize && (frameIndex < geometryStableSince + 5 ||
          (nativeW >= 0 && (nativeW != w || nativeH != h))) && frameIndex < phaseFrame + 60) return;
      const bool geometryCorrect = io.DisplaySize.x == w && io.DisplaySize.y == h &&
          viewport.width == unsigned(w) && viewport.height == unsigned(h) &&
          (nativeW < 0 || (nativeW == w && nativeH == h));
      const bool renderCorrect = width == unsigned(w) && height == unsigned(h);
      geometryMatches = geometryMatches && geometryCorrect;
      renderFollowsWindow = renderFollowsWindow && renderCorrect;
      const bool correct = w > 0 && h > 0 && geometryCorrect && renderCorrect &&
          (isResize || isEnabled == (action % 3 == 2));
      fprintf(stderr, "[ui-resize-test] action %u (%s) %s at frame %u, render %ux%u, window %dx%d, native %dx%d, display %.0fx%.0f\n",
              action, isResize ? "Resize" : "Enabled", correct ? "PASS" : "FAIL", frameIndex,
              width,height,w,h,nativeW,nativeH,io.DisplaySize.x,io.DisplaySize.y);
      if (!correct) {
        error = !renderCorrect ? "render/NR dimensions do not follow actual window" :
            !geometryCorrect ? "settled native window/ImGui/viewport dimensions differ" :
            isResize ? "actual window extent is invalid" : "SDL click did not produce expected NR state";
        done = true; return;
      }
      if (isResize) {
        ++resizesPassed;
        if (w != lastCheckedW || h != lastCheckedH) ++actualExtentChanges;
        lastCheckedW = w; lastCheckedH = h;
      } else ++clicksPassed;
      if (++action == 18) {
        if (!actualExtentChanges) error = "window manager allowed no actual window extent change";
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
           << ",\n  \"logical_clicks_passed\": " << clicksPassed
           << ",\n  \"resize_requests\": " << resizeRequests
           << ",\n  \"resizes_passed\": " << resizesPassed
           << ",\n  \"actual_extent_changes\": " << actualExtentChanges
           << ",\n  \"actions_passed\": " << action
           << ",\n  \"render_follows_window\": " << (renderFollowsWindow ? "true" : "false")
           << ",\n  \"geometry_matches\": " << (geometryMatches ? "true" : "false") << ",\n  \"completed_frames\": " << completed
           << ",\n  \"begin_frame_skips\": " << skipped << ",\n  \"samples\": [\n" << samples.str() << "\n  ]\n}\n";
    try { client::writeTextFile(output,result.str()); }
    catch (const std::exception& e) { fprintf(stderr,"[ui-resize-test] %s\n",e.what()); return false; }
    return done && error.empty();
  }
};
