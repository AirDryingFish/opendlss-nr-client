#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "client_controls.h"
#include "live_framegen.h"
#include "live_nr.h"
#include "live_backends.h"
#include "live_processing.h"
#include "live_controls.h"
#include "live_capture_probe.h"
#include "live_x11.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <SDL_syswm.h>

namespace {
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr const char* usage =
    "Usage: dlss5-live (--region X,Y,W,H|pick | --window XID|pick)\n"
    "                  [--model DIR] [--kernels DIR] [--nr 0|1] [--frame-gen 0|1] [--fg-backend flow|rife|mako] [--fg-multiplier 2|3|4] [--fps N]\n"
    "                  [--frames N] [--output FILE.ppm]\n"
    "Capture an X11 source. F1 opens settings, N toggles NR, G toggles frame generation, B selects the next backend, Esc exits.\n"
    "--fg-backend selects NVIDIA optical flow (default), RIFE v4.6 or MAKO FG. --list-fg-backends lists installed options.\n"
    "--probe-feature window|region|nr|flow|rife|mako tests the selected feature with a small input.\n"
    "--fps limits source sampling (default 30, 0 for unlimited). --frames counts source captures.\n"
    "--output saves the last displayed source frame, including NR when enabled.\n";

struct Options {
  std::string region, window, model, kernels, output, probe;
  std::string fgBackend = "flow";
  int frames = 0, fps = 30, multiplier = 2;
  bool nr = true, frameGen = false;
};

Options parseOptions(int argc, char** argv) {
  Options o;
  const auto backends = liveBackends();
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--list-fg-backends" && argc == 2) {
      for (const auto& backend : backends)
        if (backend.installed) std::printf("%s\t%s\n", backend.id, backend.label);
      std::exit(0);
    }
    if (key == "--help" || key == "-h") {
      std::fputs(usage, stdout);
      std::exit(0);
    }
    if (++i == argc) throw std::invalid_argument("missing value for " + key);
    const std::string value = argv[i];
    auto integer = [&](int minimum, int maximum) {
      try { return client::parseInteger(value, minimum, maximum); }
      catch (const std::exception&) { throw std::invalid_argument("invalid " + key + " value: " + value); }
    };
    if (key == "--region" && o.region.empty() && o.window.empty()) o.region = value;
    else if (key == "--window" && o.region.empty() && o.window.empty()) o.window = value;
    else if (key == "--region" || key == "--window") throw std::invalid_argument("choose exactly one capture source");
    else if (key == "--model") o.model = value;
    else if (key == "--kernels") o.kernels = value;
    else if (key == "--output") o.output = value;
    else if (key == "--frames") o.frames = integer(1, std::numeric_limits<int>::max());
    else if (key == "--fps") o.fps = integer(0, 1000);
    else if (key == "--nr") o.nr = integer(0, 1) != 0;
    else if (key == "--fg-multiplier") o.multiplier = integer(2, 4);
    else if (key == "--probe-feature") o.probe = value;
    else if (key == "--frame-gen") o.frameGen = integer(0, 1) != 0;
    else if (key == "--fg-backend") {
      if (std::none_of(backends.begin(), backends.end(), [&](const auto& item) { return value == item.id; }))
        throw std::invalid_argument("--fg-backend expects flow, rife or mako");
      o.fgBackend = value;
    }
    else throw std::invalid_argument("unknown argument: " + key);
  }
  if (o.probe.empty() && o.region.empty() && o.window.empty()) throw std::invalid_argument("choose --region or --window");
  return o;
}

void sdlCheck(bool success, const char* action) {
  if (!success) throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
}

// Keep the initial preview and its window decoration outside the capture rectangle if possible.
SDL_Rect previewBounds(const SDL_Rect& source, bool capturingRoot) {
  SDL_Rect best{}, fallback{};
  double bestScale = 0.0;
  auto fit = [&](SDL_Rect area, double& scale) {
    const int side = 12, top = 44;
    scale = std::min({1.0, double(area.w - 2 * side) / source.w, double(area.h - top - side) / source.h});
    if (scale <= 0) return SDL_Rect{};
    const int w = std::max(1, int(source.w * scale));
    const int h = std::max(1, int(source.h * scale));
    return SDL_Rect{area.x + (area.w - w) / 2, area.y + top + (area.h - top - side - h) / 2, w, h};
  };
  for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i) {
    SDL_Rect display{}, overlap{};
    if (SDL_GetDisplayUsableBounds(i, &display) && SDL_GetDisplayBounds(i, &display)) continue;
    double scale;
    auto consider = [&](SDL_Rect freeArea) {
      auto rect = fit(freeArea, scale);
      if (scale > bestScale) { bestScale = scale; best = rect; }
    };
    if (i == 0) fallback = fit(display, scale);
    if (!SDL_IntersectRect(&source, &display, &overlap)) {
      consider(display);
      continue;
    }
    const int right = overlap.x + overlap.w, bottom = overlap.y + overlap.h;
    consider({display.x, display.y, overlap.x - display.x, display.h});
    consider({right, display.y, display.x + display.w - right, display.h});
    consider({display.x, display.y, display.w, overlap.y - display.y});
    consider({display.x, bottom, display.w, display.y + display.h - bottom});
  }
  if (best.w >= std::min(128, source.w) && best.h >= std::min(72, source.h)) return best;
  if (capturingRoot)
    std::fprintf(stderr, "[live] Preview may overlap the screen region. Move it to another display or use --window.\n");
  if (fallback.w > 0 && fallback.h > 0) return fallback;
  return {SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, source.w, source.h};
}

void savePpm(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
  std::ofstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot create " + path);
  file << "P6\n" << w << " " << h << "\n255\n";
  std::vector<char> row(size_t(w) * 3);
  for (int y = 0; y < h; ++y) {
    const uint8_t* pixel = rgba.data() + size_t(y) * w * 4;
    for (int x = 0; x < w; ++x, pixel += 4)
      std::copy_n(pixel, 3, row.data() + size_t(x) * 3);
    file.write(row.data(), row.size());
  }
  file.close();
  if (!file) throw std::runtime_error("failed writing " + path);
}

struct SdlVideo {
  SdlVideo() { sdlCheck(SDL_Init(SDL_INIT_VIDEO) == 0, "initialize SDL video"); }
  ~SdlVideo() { SDL_Quit(); }
};

template<class T, void (*destroy)(T*)>
using SdlObject = std::unique_ptr<T, decltype(destroy)>;

struct Processed {
  const uint8_t* pixels = nullptr;
  bool hasIntermediates = false;
  Clock::duration interpolationTime{};
  std::string flowError;
};

// The caller joins each task before submitting another one or changing its input buffers.
// While the GPU processes the next pair, the SDL thread can present the previous endpoint.
class FrameWorker {
 public:
  FrameWorker() : thread_([this] {
    for (;;) {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [&] { return stopping_ || task_.valid(); });
      if (!task_.valid()) return;
      auto task = std::move(task_);
      lock.unlock();
      task();
    }
  }) {}
  ~FrameWorker() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    ready_.notify_one();
    thread_.join();
  }
  std::future<Processed> submit(std::packaged_task<Processed()> task) {
    auto result = task.get_future();
    { std::lock_guard lock(mutex_); task_ = std::move(task); }
    ready_.notify_one();
    return result;
  }
 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::packaged_task<Processed()> task_;
  bool stopping_ = false;
  std::thread thread_;
};

void run(Options o) {
  const fs::path build = fs::canonical("/proc/self/exe").parent_path().parent_path();
  if (o.model.empty()) {
    const char* envModel = std::getenv("DLSS5VK_MODEL");
    o.model = envModel && *envModel ? fs::path(envModel) : build.parent_path() / "models/nr";
  }
  if (o.kernels.empty()) o.kernels = build / "shaders";
  const std::string demoShaders = build / "data/shaders";

  if (!o.probe.empty()) {
    if (o.probe == "window" || o.probe == "region") {
      probeLiveCapture(o.probe == "window");
      std::printf("X11 %s capture OK\n", o.probe.c_str());
    } else {
      probeLiveProcessing(o.probe, o.multiplier, o.model, o.kernels, demoShaders);
    }
    return;
  }
  SdlVideo sdl;
  LiveX11 source(o.region, o.window);
  const SDL_Rect region = {source.x(), source.y(), source.width(), source.height()};
  const SDL_Rect bounds = previewBounds(region, !o.region.empty());
  SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
  auto openWindow = [&](Uint32 extraFlags) {
    return SDL_CreateWindow("DLSS 5 Live", bounds.x, bounds.y, bounds.w, bounds.h,
                            SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | extraFlags);
  };
  // SDL's GL renderer otherwise destroys and remaps the initial X11 window.
  SdlObject<SDL_Window, SDL_DestroyWindow> window(openWindow(SDL_WINDOW_OPENGL), SDL_DestroyWindow);
  if (!window) window.reset(openWindow(0));
  sdlCheck(bool(window), "open preview");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SdlObject<SDL_Renderer, SDL_DestroyRenderer> renderer(
      SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC), SDL_DestroyRenderer);
  if (!renderer) renderer.reset(SDL_CreateRenderer(window.get(), -1, 0));
  sdlCheck(bool(renderer), "create preview renderer");
  SDL_SysWMinfo native{};
  SDL_VERSION(&native.version);
  sdlCheck(SDL_GetWindowWMInfo(window.get(), &native), "identify the preview window");
  if (native.subsystem != SDL_SYSWM_X11) throw std::runtime_error("Live preview requires X11");
  source.setPreviewWindow(native.info.x11.window);
  SDL_RendererInfo rendererInfo{};
  sdlCheck(!SDL_GetRendererInfo(renderer.get(), &rendererInfo), "query preview renderer");
  const bool vsync = (rendererInfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
  std::fprintf(stderr, "[live] preview renderer=%s, vertical sync %s\n", rendererInfo.name,
               vsync ? "on" : "unavailable (unsynchronized preview)");
  sdlCheck(!SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, SDL_ALPHA_OPAQUE), "set preview background");
  SdlObject<SDL_Texture, SDL_DestroyTexture> texture(nullptr, SDL_DestroyTexture);
  std::unique_ptr<LiveNr> nr;
  std::unique_ptr<LiveInterpolator> frameGen;
  const auto backends = liveBackends();
  auto selected = std::find_if(backends.begin(), backends.end(),
      [&](const auto& item) { return o.fgBackend == item.id; });
  LiveControls controls(window.get(), renderer.get(), backends);
  std::vector<uint8_t> input, previousInput, previousEnhanced, generatedBatch, pendingBatch, lastFrame;
  int w = 0, h = 0, savedWidth = 0, savedHeight = 0;
  int64_t captured = 0, duplicates = 0, generated = 0, presented = 0;
  int64_t lastCaptured = 0, lastPresented = 0;
  bool running = true, minimized = false, waiting = false, textureReady = false;
  bool busy = false, overlapping = false, refreshPanel = true;
  unsigned historyVersion = 0;
  Clock::time_point lastChangedAt{}, retryGenerationAt{};
  Clock::time_point lastNrAt{};
  const auto noDeadline = Clock::time_point::max();
  auto pendingAt = noDeadline, nextCapture = Clock::now();
  Clock::duration pendingStep{};
  int nextPhase = 0, pendingPhases = 0;
  double captureFps = 0.0, presentFps = 0.0, networkMs = 0.0;
  std::string processingError;
  FrameWorker worker;

  auto resetHistory = [&] {
    ++historyVersion;
    pendingAt = noDeadline;
    lastNrAt = {};
    retryGenerationAt = {};
    if (busy) return; // The in-flight pair still reads its endpoints.
    previousInput.clear();
    previousEnhanced.clear();
    lastChangedAt = {};
  };
  auto settings = [&] {
    return LiveSettings{o.nr, o.frameGen, o.multiplier, o.fps, selected->id};
  };
  auto drawPreview = [&] {
    std::string status = overlapping ? "Paused: move preview outside the capture region" :
                         waiting ? "Waiting for capture source" : processingError;
    controls.present(textureReady ? texture.get() : nullptr, settings(),
                     {w, h, captureFps, presentFps, networkMs, vsync, status});
    refreshPanel = false;
  };
  bool reloadFrameGen = false;
  auto applySettings = [&](const LiveSettings& edit) {
    if (edit == settings()) return;
    reloadFrameGen |= edit.backend != selected->id || edit.multiplier != o.multiplier || !edit.frameGen;
    selected = std::find_if(backends.begin(), backends.end(),
        [&](const auto& item) { return edit.backend == item.id; });
    if (edit.fps != o.fps) nextCapture = Clock::now();
    o.nr = edit.nr;
    o.frameGen = edit.frameGen;
    o.multiplier = edit.multiplier;
    o.fps = edit.fps;
    if (!o.nr) networkMs = 0;
    processingError.clear();
    refreshPanel = true;
    resetHistory();
    std::fprintf(stderr, "[live] settings NR=%s FG=%s backend=%s multiplier=%d fps=%d\n",
                 o.nr ? "on" : "off", o.frameGen ? "on" : "off", selected->id, o.multiplier, o.fps);
  };
  auto disableFrameGen = [&](const std::exception& e) {
    processingError = e.what();
    std::fprintf(stderr, "[live] %s frame generation disabled: %s\n", selected->label, e.what());
    o.frameGen = false;
    frameGen.reset();
    resetHistory();
    refreshPanel = true;
  };
  auto pumpEvents = [&] {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      controls.event(event);
      if (event.type == SDL_QUIT) running = false;
      if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        auto edit = settings();
        if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
        if (event.key.keysym.sym == SDLK_b) {
          auto next = selected;
          do {
            if (++next == backends.end()) next = backends.begin();
          } while (next != selected && !next->installed);
          edit.backend = next->id;
        }
        if (event.key.keysym.sym == SDLK_n) edit.nr = !edit.nr;
        if (event.key.keysym.sym == SDLK_g) edit.frameGen = !edit.frameGen;
        applySettings(edit);
      }
      if (event.type != SDL_WINDOWEVENT) continue;
      switch (event.window.event) {
        case SDL_WINDOWEVENT_CLOSE: running = false; break;
        case SDL_WINDOWEVENT_MINIMIZED: minimized = true; resetHistory(); break;
        case SDL_WINDOWEVENT_RESTORED: minimized = false; resetHistory(); refreshPanel = true; break;
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_EXPOSED: refreshPanel = true; break;
      }
    }
    if (running && !minimized && (refreshPanel || controls.needsRedraw())) drawPreview();
    if (auto edit = controls.takeEdits()) applySettings(*edit);
  };

  // Resizing the preview only changes SDL's aspect-preserving presentation.
  auto prepare = [&](bool startup) {
    if (source.width() != w || source.height() != h) {
      resetHistory();
      frameGen.reset();
      nr.reset();
      networkMs = 0;
      w = source.width();
      h = source.height();
      textureReady = false;
      texture.reset(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h));
      sdlCheck(bool(texture), "allocate preview texture");
      refreshPanel = true;
    }
    if (reloadFrameGen) {
      frameGen.reset();
      reloadFrameGen = false;
    }
    if (o.nr && !nr) {
      try {
        nr = std::make_unique<LiveNr>(w, h, o.model, o.kernels, demoShaders);
        std::fprintf(stderr, "[live] NR %dx%d on %s\n", w, h, nr->deviceName().c_str());
      } catch (const std::exception& e) {
        if (startup) throw;
        processingError = e.what();
        std::fprintf(stderr, "[live] NR disabled: %s\n", e.what());
        o.nr = false;
        refreshPanel = true;
        resetHistory();
      }
    }
    if (o.frameGen && !frameGen) {
      try {
        frameGen = makeLiveInterpolator(selected->id, w, h, o.multiplier);
        std::fprintf(stderr, "[live] %s frame generation %dx at %dx%d\n", selected->label, o.multiplier, w, h);
      } catch (const std::exception& e) {
        if (startup) throw;
        disableFrameGen(e);
      }
    }
  };
  auto present = [&](const uint8_t* pixels) {
    sdlCheck(!SDL_UpdateTexture(texture.get(), nullptr, pixels, w * 4), "upload preview");
    textureReady = true;
    drawPreview();
    ++presented;
  };
  auto presentReal = [&](const uint8_t* pixels) {
    present(pixels);
    if (!o.output.empty()) {
      savedWidth = w;
      savedHeight = h;
      lastFrame.assign(pixels, pixels + size_t(w) * h * 4);
    }
  };
  prepare(true);
  const auto started = Clock::now();
  auto intervalStart = started;
  nextCapture = started;
  auto finishPair = [&] {
    presentReal(o.nr ? previousEnhanced.data() : previousInput.data());
    pendingAt = noDeadline;
  };
  // Each phase has an absolute deadline. A slow display can skip synthetic frames
  // without moving the endpoint or delaying the next capture by an entire batch.
  auto presentDue = [&](Clock::time_point now) {
    if (pendingAt > now) return;
    const auto missed = (now - pendingAt) / pendingStep;
    if (missed >= pendingPhases - nextPhase) {
      finishPair();
    } else {
      nextPhase += int(missed);
      present(pendingBatch.data() + size_t(nextPhase++) * w * h * 4);
      ++generated;
      pendingAt += (missed + 1) * pendingStep;
    }
  };
  auto waitForCapture = [&](Clock::time_point deadline, unsigned version) {
    for (;;) {
      pumpEvents();
      if (!running || minimized || version != historyVersion) return false;
      const auto now = Clock::now();
      if (now >= deadline || (deadline == noDeadline && pendingAt == noDeadline)) return true;
      if (now >= pendingAt) {
        presentDue(now);
      } else {
        std::this_thread::sleep_until(std::min({deadline, pendingAt, now + std::chrono::milliseconds(8)}));
      }
    }
  };
  while (running && (!o.frames || captured < o.frames)) {
    pumpEvents();
    if (!running) break;
    if (minimized) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      continue;
    }
    if (!waitForCapture(nextCapture, historyVersion)) continue;
    const Clock::duration period = o.fps ? std::chrono::nanoseconds(1000000000 / o.fps) : Clock::duration::zero();
    const auto maxPairGap = std::max(Clock::duration(std::chrono::milliseconds(250)), 3 * period);
    const auto captureTime = Clock::now();
    const auto read = source.read(input);
    const bool covered = read == LiveX11::ReadResult::PreviewOverlap;
    if (covered != overlapping) {
      overlapping = covered;
      resetHistory();
      refreshPanel = true;
      if (overlapping)
        std::fprintf(stderr, "[live] Capture paused: move the preview outside the region, or capture an application window.\n");
    }
    if (overlapping) {
      nextCapture = captureTime + std::chrono::milliseconds(75);
      continue;
    }
    if (read == LiveX11::ReadResult::Unavailable) {
      if (!waiting) {
        waiting = true;
        std::fputs("[live] waiting for source\n", stderr);
        resetHistory();
        captureFps = presentFps = 0;
        refreshPanel = true;
      }
      nextCapture = captureTime + std::chrono::milliseconds(100);
      continue;
    }
    if (waiting) { waiting = false; refreshPanel = true; }
    prepare(false);
    ++captured;
    // Keep sampling on the source clock even when vsync or inference finishes late.
    const auto captureSlot = o.fps ? nextCapture + period * ((captureTime - nextCapture) / period) : captureTime;
    nextCapture = captureSlot + period;

    if (frameGen && input == previousInput) {
      ++duplicates;
      presentDue(Clock::now());
      // For an unlimited static source there is no display or neural work to pace the poll.
      if (!o.fps) nextCapture = captureTime + std::chrono::milliseconds(1);
      // A source swapping at our sampling rate can straddle the same deadline
      // on every frame. Nudge the sampling phase when a moving image repeats.
      else if (captureTime - lastChangedAt < period + period / 2)
        nextCapture += std::min(period / 16, Clock::duration(std::chrono::milliseconds(1)));
    } else {
      if (o.nr) {
        // Reset only between jobs; a keyboard or window event can also arrive during processing.
        if (lastNrAt == Clock::time_point{} || captureTime - lastNrAt > maxPairGap) nr->resetHistory();
        lastNrAt = captureTime;
      }
      const auto pairInterval = captureTime - lastChangedAt;
      const bool canInterpolate = frameGen && !previousInput.empty() && pairInterval <= maxPairGap &&
                                  captureTime >= retryGenerationAt;
      auto process = [&, applyNr = o.nr, factor = o.multiplier, canInterpolate] {
        Processed result;
        result.pixels = applyNr ? nr->process(input.data()) : input.data();
        if (canInterpolate) {
          const uint8_t* previous = applyNr ? previousEnhanced.data() : previousInput.data();
          generatedBatch.resize(input.size() * (factor - 1));
          const auto start = Clock::now();
          try {
            result.hasIntermediates = frameGen->interpolate(previous, result.pixels, generatedBatch.data(),
                                                       applyNr ? previousInput.data() : nullptr,
                                                       applyNr ? input.data() : nullptr);
          }
          catch (const std::exception& e) { result.flowError = e.what(); }
          result.interpolationTime = Clock::now() - start;
        }
        return result;
      };

      Processed result;
      if (pendingAt != noDeadline) {
        // The worker writes the new batch while the SDL thread finishes presenting the prior one.
        const unsigned version = historyVersion;
        busy = true;
        auto job = worker.submit(std::packaged_task<Processed()>(process));
        finishPair();
        while (job.wait_for(std::chrono::milliseconds(4)) != std::future_status::ready) pumpEvents();
        result = job.get();
        busy = false;
        if (!running || version != historyVersion) {
          resetHistory();
          continue;
        }
      } else {
        result = process();
      }
      if (o.nr) networkMs = nr->networkMs();
      if (!result.flowError.empty()) disableFrameGen(std::runtime_error(result.flowError));

      if (result.hasIntermediates) {
        pendingStep = std::max(Clock::duration(1), pairInterval / o.multiplier);
        pendingBatch.swap(generatedBatch);
        nextPhase = 0;
        pendingPhases = o.multiplier - 1;
        pendingAt = captureSlot;
      } else {
        presentReal(result.pixels);
      }
      if (frameGen) {
        lastChangedAt = captureTime;
        if (o.nr) previousEnhanced.assign(result.pixels, result.pixels + input.size());
        previousInput.swap(input);
        // If inference alone exceeds the source interval, pass through real frames
        // for a short interval before testing the GPU budget again.
        if (o.fps && result.interpolationTime > pairInterval)
          retryGenerationAt = Clock::now() + 8 * pairInterval;
        presentDue(Clock::now());
      }
    }
    const auto end = Clock::now();
    const double seconds = std::chrono::duration<double>(end - intervalStart).count();
    if (seconds >= 1.0) {
      captureFps = double(captured - lastCaptured) / seconds;
      presentFps = double(presented - lastPresented) / seconds;
      lastCaptured = captured;
      lastPresented = presented;
      intervalStart = end;
      refreshPanel = true;
    }
  }
  if (running && pendingAt != noDeadline) waitForCapture(noDeadline, historyVersion);
  const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
  std::fprintf(stderr, "[live] frames=%lld size=%dx%d NR=%s FG=%s elapsed=%.3f s fps=%.2f "
               "captured=%lld duplicates=%lld generated=%lld presented=%lld present_fps=%.2f\n",
               static_cast<long long>(captured), w, h, o.nr ? "on" : "off", o.frameGen ? "on" : "off", seconds,
               seconds > 0 ? captured / seconds : 0.0, static_cast<long long>(captured), static_cast<long long>(duplicates),
               static_cast<long long>(generated), static_cast<long long>(presented), seconds > 0 ? presented / seconds : 0.0);
  if (!o.output.empty()) {
    if (lastFrame.empty()) throw std::runtime_error("no displayed frame to save");
    savePpm(o.output, savedWidth, savedHeight, lastFrame);
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    run(parseOptions(argc, argv));
    return 0;
  } catch (const LiveX11::SelectionCancelled& e) {
    std::fprintf(stderr, "dlss5-live: %s\n", e.what());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "dlss5-live: %s\n", e.what());
    return 1;
  }
}
