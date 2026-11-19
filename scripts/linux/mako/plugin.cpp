#include "../../../demo/live_fg_plugin_abi.h"
#include "exchange.h"
#include "external.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace {
// The official runtime takes ownership of the descriptors on constructor entry.
// Keep ownership here until all exports and allocations have succeeded.
struct Fds {
    int a = -1, b = -1, timeline = -1;
    std::vector<int> outputs;
    ~Fds() {
        for (int fd : {a, b, timeline}) if (fd >= 0) ::close(fd);
        for (int fd : outputs) if (fd >= 0) ::close(fd);
    }
};

class Session {
public:
    Session(unsigned w, unsigned h, unsigned multiplier, const char* directory)
        : exchange_(w, h, multiplier - 1), times_(multiplier - 1) {
        for (unsigned i = 0; i < times_.size(); ++i) times_[i] = float(i + 1) / multiplier;
        const auto package = mfg::readPackageDirectory(directory);
        const auto devices = mfg::devices();
        const auto uuid = exchange_.deviceUuid();
        const auto device = std::find_if(devices.begin(), devices.end(),
            [&](const auto& d) { return d.device_uuid == uuid; });
        if (device == devices.end()) throw std::runtime_error("MAKO runtime cannot import images on the capture GPU");
        Fds files;
        files.a = exchange_.exportImage(0);
        files.b = exchange_.exportImage(1);
        files.outputs.resize(times_.size(), -1);
        for (unsigned i = 0; i < times_.size(); ++i) files.outputs[i] = exchange_.exportImage(2 + i);
        files.timeline = exchange_.exportTimeline();
        // Release the output FD together with the sources, after new allocates the context.
        context_.reset(new mfg::ExternalContext(package, device->index, exchange_.supportsHalfStorage(), w, h,
            std::exchange(files.a,-1), std::exchange(files.b,-1),
            std::move(files.outputs), std::exchange(files.timeline,-1)));
        context_->setMotionPeriod(1);
    }

    void interpolate(const uint8_t* previous, const uint8_t* current, uint8_t* output) {
        // A cut, skipped capture, or NR toggle can break adjacency. Prime the supplied
        // previous endpoint, so cached pyramids never belong to a different pair.
        if (!frames_ || !exchange_.lastUploadMatches(previous)) submit(previous, {});
        submit(current, times_);
        exchange_.download(output, nextSignal_ - 1);
    }

private:
    void submit(const uint8_t* input, std::span<const float> times) {
        exchange_.upload(frames_ % 2, input, nextSignal_, !frames_);
        context_->schedule(times);
        if (!context_->ready(10'000'000'000ull)) throw std::runtime_error("Timed out waiting for MAKO FG");
        nextSignal_ += 1 + times.size();
        ++frames_;
    }
    MakoExchange exchange_;
    std::vector<float> times_;
    std::unique_ptr<mfg::ExternalContext> context_;
    uint64_t frames_ = 0, nextSignal_ = 1;
};

void* openSession(unsigned w, unsigned h, unsigned multiplier, const char* model, char* error, size_t size) {
    try {
        if (multiplier < 2 || multiplier > 4) throw std::runtime_error("MAKO multiplier must be 2, 3 or 4");
        return new Session(w, h, multiplier, model);
    }
    catch (const std::exception& e) { std::snprintf(error, size, "%s", e.what()); return nullptr; }
}
int interpolate(void* opaque, const uint8_t* prev, const uint8_t* next, uint8_t* output, char* error, size_t size) {
    try { static_cast<Session*>(opaque)->interpolate(prev, next, output); return 0; }
    catch (const std::exception& e) { std::snprintf(error, size, "%s", e.what()); return -1; }
}
void* openMidpoint(unsigned w, unsigned h, const char* model, char* error, size_t size) {
    return openSession(w, h, 2, model, error, size);
}
void closeSession(void* opaque) { delete static_cast<Session*>(opaque); }
} // namespace

extern "C" __attribute__((visibility("default"))) const LiveFgPluginV1* live_mako_plugin_v1() {
    static const LiveFgPluginV1 api{openMidpoint, interpolate, closeSession};
    return &api;
}

extern "C" __attribute__((visibility("default"))) const LiveFgPluginV2* live_mako_plugin_v2() {
    static const LiveFgPluginV2 api{openSession, interpolate, closeSession};
    return &api;
}
