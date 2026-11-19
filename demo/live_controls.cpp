#include "live_controls.h"

#include <SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

LiveControls::LiveControls(SDL_Window* window, SDL_Renderer* renderer, const std::array<LiveBackend, 3>& backends)
    : renderer_(renderer), backends_(backends) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const bool platform = ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    if (!platform || !ImGui_ImplSDLRenderer2_Init(renderer)) {
        if (platform) ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("Cannot initialize the Live settings panel");
    }
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 8;
    style.FrameRounding = 4;
    style.PopupRounding = 4;
    style.WindowPadding = {12, 10};
    style.FramePadding = {7, 5};
    style.ItemSpacing = {8, 7};
    showButtonUntil_ = Clock::now() + std::chrono::seconds(4);
}

LiveControls::~LiveControls() {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void LiveControls::event(const SDL_Event& event) {
    ImGui_ImplSDL2_ProcessEvent(&event);
    const auto now = Clock::now();
    if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F1 && !event.key.repeat) {
        open_ = !open_;
    } else if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN ||
               event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEWHEEL) {
        showButtonUntil_ = now + std::chrono::seconds(3);
    } else if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP && event.type != SDL_WINDOWEVENT) {
        return;
    }
    // ImGui can deliver a press and its release in successive UI frames.
    repaintUntil_ = now + std::chrono::milliseconds(60);
}

bool LiveControls::needsRedraw() const {
    const auto now = Clock::now();
    return now >= nextRepaint_ && (now < repaintUntil_ || (toolbarExpires_ && now >= showButtonUntil_));
}

std::optional<LiveSettings> LiveControls::takeEdits() {
    return std::exchange(edits_, std::nullopt);
}

void LiveControls::drawPanel(const LiveSettings& settings, const LiveStatus& status) {
    const auto now = Clock::now();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    constexpr auto flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    const float margin = std::min(10.f, std::min(size.x, size.y) * .04f);
    ImGui::SetNextWindowPos({size.x - margin, margin}, ImGuiCond_Always, {1, 0});
    if (!open_) {
        toolbarExpires_ = status.message.empty() && now < showButtonUntil_;
        if (!toolbarExpires_ && status.message.empty()) return;
        ImGui::SetNextWindowBgAlpha(.86f);
        ImGui::Begin("Live toolbar", nullptr, flags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
        if (ImGui::Button("Settings (F1)")) open_ = true;
        if (!status.message.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(70.f, std::min(230.f, size.x - 48)));
            ImGui::TextUnformatted(status.message.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
        return;
    }
    toolbarExpires_ = false;
    ImGui::SetNextWindowSize({std::max(1.f, std::min(312.f, size.x - 2 * margin)), 0}, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints({1, 1}, {std::max(1.f, size.x - 2 * margin), std::max(1.f, size.y - 2 * margin)});
    ImGui::SetNextWindowBgAlpha(.97f);
    ImGui::Begin("Live settings", &open_, flags | ImGuiWindowFlags_AlwaysAutoResize);
    LiveSettings updated = settings;
    ImGui::Checkbox("Neural rendering (NR)", &updated.nr);
    const bool anyBackend = std::any_of(backends_.begin(), backends_.end(), [](const auto& b) { return b.installed; });
    ImGui::BeginDisabled(!anyBackend);
    ImGui::Checkbox("Frame generation", &updated.frameGen);
    if (ImGui::BeginTable("Capture settings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](const char* label) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
        };
        row("Algorithm");
        const char* current = "Unavailable";
        for (const auto& b : backends_) if (b.id == updated.backend && b.installed) current = b.label;
        if (ImGui::BeginCombo("##algorithm", current)) {
            for (const auto& b : backends_) {
                if (!b.installed) continue;
                const bool selected = updated.backend == b.id;
                if (ImGui::Selectable(b.label, selected)) updated.backend = b.id;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        row("Multiplier");
        const char* labels[] = {"2x", "3x", "4x"};
        int index = updated.multiplier - 2;
        if (ImGui::Combo("##multiplier", &index, labels, 3)) updated.multiplier = index + 2;
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Capture FPS");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    const std::string rate = updated.fps ? std::to_string(updated.fps) : "Unlimited";
    if (ImGui::BeginCombo("##fps", rate.c_str())) {
        for (int fps : {24, 30, 60, 120, 0}) {
            const std::string text = fps ? std::to_string(fps) : "Unlimited";
            const bool selected = updated.fps == fps;
            if (ImGui::Selectable(text.c_str(), selected)) updated.fps = fps;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();
    ImGui::Text("%d x %d     VSync %s", status.width, status.height, status.vsync ? "on" : "off");
    ImGui::Text("Capture %.1f FPS    Present %.1f FPS", status.captureFps, status.presentFps);
    if (settings.nr && status.networkMs > 0) ImGui::Text("NR GPU %.2f ms", status.networkMs);
    if (!status.message.empty()) ImGui::TextWrapped("%s", status.message.c_str());
    ImGui::End();
    if (updated != settings) edits_ = std::move(updated);
}

void LiveControls::present(SDL_Texture* texture, const LiveSettings& settings, const LiveStatus& status) {
    int width, height;
    if (SDL_GetRendererOutputSize(renderer_, &width, &height) != 0 || SDL_RenderClear(renderer_) != 0)
        throw std::runtime_error(SDL_GetError());
    if (texture) {
        int tw, th;
        if (SDL_QueryTexture(texture, nullptr, nullptr, &tw, &th) != 0) throw std::runtime_error(SDL_GetError());
        const double scale = std::min(double(width) / tw, double(height) / th);
        const int w = int(std::lround(tw * scale)), h = int(std::lround(th * scale));
        SDL_Rect dest{(width - w) / 2, (height - h) / 2, w, h};
        if (SDL_RenderCopy(renderer_, texture, nullptr, &dest) != 0) throw std::runtime_error(SDL_GetError());
    }
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    drawPanel(settings, status);
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
    SDL_RenderPresent(renderer_);
    nextRepaint_ = Clock::now() + std::chrono::milliseconds(8);
}
