#pragma once
#include "live_fg_plugin_abi.h"
#include <dlfcn.h>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>

class LiveFgPlugin {
public:
    static bool installed(const char* libraryPath, const char* modelPath,
                          std::initializer_list<const char*> modelFiles) {
        std::error_code error;
        const auto root = std::filesystem::read_symlink("/proc/self/exe", error).parent_path().parent_path();
        if (error || !std::filesystem::is_regular_file(root / libraryPath, error)) return false;
        for (const char* file : modelFiles)
            if (!std::filesystem::is_regular_file(root / modelPath / file, error)) return false;
        return true;
    }

    LiveFgPlugin(const char* libraryPath, const char* backend, const char* modelPath, unsigned width,
                 unsigned height, unsigned multiplier) {
        if (multiplier < 2 || multiplier > 4) throw std::invalid_argument("frame generation multiplier must be 2, 3 or 4");
        const auto root = std::filesystem::read_symlink("/proc/self/exe").parent_path().parent_path();
        library_.reset(dlopen((root / libraryPath).c_str(), RTLD_NOW | RTLD_LOCAL));
        if (!library_) throw std::runtime_error(std::string("Open interpolation plugin: ") + dlerror());
        const auto symbol = std::string("live_") + backend + "_plugin_v";
        const auto query = reinterpret_cast<LiveFgPluginQueryV2>(dlsym(library_.get(), (symbol + "2").c_str()));
        char error[384]{};
        if (query) {
            const auto* api = query();
            generate_ = api->interpolate;
            close_ = api->close;
            session_ = api->open(width, height, multiplier, (root / modelPath).c_str(), error, sizeof(error));
        } else {
            const auto legacy = reinterpret_cast<LiveFgPluginQuery>(dlsym(library_.get(), (symbol + "1").c_str()));
            if (!legacy || multiplier != 2)
                throw std::runtime_error(std::string("Update the interpolation plugin with scripts/linux/prepare-") + backend + ".sh");
            const auto* api = legacy();
            generate_ = api->midpoint;
            close_ = api->close;
            session_ = api->open(width, height, (root / modelPath).c_str(), error, sizeof(error));
        }
        if (!session_) throw std::runtime_error(error);
    }

    ~LiveFgPlugin() { if (session_) close_(session_); }

    bool interpolate(const uint8_t* previous, const uint8_t* current, uint8_t* intermediates) {
        char error[384]{};
        if (generate_(session_, previous, current, intermediates, error, sizeof(error)) != 0)
            throw std::runtime_error(error);
        return true;
    }

private:
    std::unique_ptr<void, int (*)(void*)> library_{nullptr, dlclose};
    decltype(LiveFgPluginV2::interpolate) generate_ = nullptr;
    decltype(LiveFgPluginV2::close) close_ = nullptr;
    void* session_ = nullptr;
};
