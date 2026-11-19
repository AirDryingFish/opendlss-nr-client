#include "../../../demo/live_fg_plugin_abi.h"
#include "rife.h"
#include "../../../src/sha256.h"

#include <filesystem>
#include <fstream>

#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>

namespace {
struct Session {
    int w, h;
    unsigned multiplier;
    RIFE network;
    Session(int width, int height, int device, unsigned factor) : w(width), h(height), multiplier(factor), network(device, false, false, false, 1, false, true) {}
};

void verifyModel(const std::filesystem::path& path, size_t size, const char* expected) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (input.tellg() == static_cast<std::streamoff>(size)) {
        std::vector<uint8_t> bytes(size);
        input.seekg(0);
        if (input.read(reinterpret_cast<char*>(bytes.data()), size) && sha256Hex(bytes.data(), size) == expected)
            return;
    }
    throw std::runtime_error("Invalid RIFE v4.6 resource: " + path.string() + ". Run scripts/linux/prepare-rife.sh.");
}

void* openSession(unsigned w, unsigned h, unsigned multiplier, const char* model, char* error, size_t size) {
    try {
        if (multiplier < 2 || multiplier > 4) throw std::runtime_error("RIFE multiplier must be 2, 3 or 4");
        // The private ncnn runtime owns its process-wide Vulkan instance and destroys it on unload.
        if (ncnn::create_gpu_instance() != 0) throw std::runtime_error("Cannot initialize ncnn Vulkan");
        const int device = ncnn::get_default_gpu_index();
        if (device < 0) throw std::runtime_error("No Vulkan GPU for RIFE");
        const auto& info = ncnn::get_gpu_info(device);
        if (info.type() == 3 || !info.support_fp16_storage() || !info.support_int8_storage())
            throw std::runtime_error("RIFE requires a Vulkan GPU with FP16 and 8-bit storage");
        if (!w || !h || w > INT32_MAX / 4 / h)
            throw std::runtime_error("Capture dimensions exceed the RIFE image size limit");
        verifyModel(std::filesystem::path(model) / "flownet.param", 16532,
                    "724569596BCD1E7B9FA50455C604777EBED99746D2EF40AA86E31B5725F1053C");
        verifyModel(std::filesystem::path(model) / "flownet.bin", 10614320,
                    "F334ED2260149CE0188A6DCF049844E8B0CDD912E01CBCFB63553157D2508958");
        auto session = std::make_unique<Session>(w, h, device, multiplier);
        if (session->network.load(model) != 0)
            throw std::runtime_error("Cannot load RIFE v4.6 model. Run scripts/linux/prepare-rife.sh.");
        return session.release();
    } catch (const std::exception& e) {
        std::snprintf(error, size, "%s", e.what());
        return nullptr;
    }
}

int interpolate(void* state, const uint8_t* prev, const uint8_t* next, uint8_t* output, char* error, size_t size) {
    try {
        auto& session = *static_cast<Session*>(state);
        ncnn::Mat a(session.w, session.h, const_cast<uint8_t*>(prev), size_t(4), 1);
        ncnn::Mat b(session.w, session.h, const_cast<uint8_t*>(next), size_t(4), 1);
        const size_t frameBytes = size_t(session.w) * session.h * 4;
        for (unsigned phase = 1; phase < session.multiplier; ++phase) {
            ncnn::Mat out(session.w, session.h, output + frameBytes * (phase - 1), size_t(4), 1);
            if (session.network.process_v4(a, b, float(phase) / session.multiplier, out) != 0)
                throw std::runtime_error("RIFE Vulkan inference failed");
        }
        return 0;
    } catch (const std::exception& e) {
        std::snprintf(error, size, "%s", e.what());
    }
    return -1;
}

void* openMidpoint(unsigned w, unsigned h, const char* model, char* error, size_t size) {
    return openSession(w, h, 2, model, error, size);
}

void closeSession(void* state) { delete static_cast<Session*>(state); }
} // namespace

extern "C" __attribute__((visibility("default"))) const LiveFgPluginV1* live_rife_plugin_v1() {
    static const LiveFgPluginV1 api{openMidpoint, interpolate, closeSession};
    return &api;
}

extern "C" __attribute__((visibility("default"))) const LiveFgPluginV2* live_rife_plugin_v2() {
    static const LiveFgPluginV2 api{openSession, interpolate, closeSession};
    return &api;
}
