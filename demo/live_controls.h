#pragma once

#include "live_backends.h"

#include <chrono>
#include <optional>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
union SDL_Event;

struct LiveSettings {
    bool nr, frameGen;
    int multiplier, fps;
    std::string backend;
    bool operator==(const LiveSettings&) const = default;
};

struct LiveStatus {
    int width, height;
    double captureFps, presentFps, networkMs;
    bool vsync;
    std::string message;
};

class LiveControls {
public:
    LiveControls(SDL_Window* window, SDL_Renderer* renderer, const std::array<LiveBackend, 3>& backends);
    ~LiveControls();
    LiveControls(const LiveControls&) = delete;
    LiveControls& operator=(const LiveControls&) = delete;

    void event(const SDL_Event& event);
    // UI input must also repaint while a static or unavailable source produces no frames.
    bool needsRedraw() const;
    void present(SDL_Texture* texture, const LiveSettings& settings, const LiveStatus& status);
    std::optional<LiveSettings> takeEdits();

private:
    void drawPanel(const LiveSettings& settings, const LiveStatus& status);
    using Clock = std::chrono::steady_clock;
    SDL_Renderer* renderer_;
    std::array<LiveBackend, 3> backends_;
    std::optional<LiveSettings> edits_;
    Clock::time_point repaintUntil_{}, nextRepaint_{}, showButtonUntil_{};
    bool open_ = false, toolbarExpires_ = false;
};
