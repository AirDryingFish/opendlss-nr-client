#include "live_framegen.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>

// Histograms survive pans and translations better than an unaligned pixel difference.
// Sample a regular grid to keep this check independent of the capture resolution.
bool liveSceneCut(const uint8_t* a, const uint8_t* b, unsigned w, unsigned h) {
    std::array<int, 48> histogramDelta{};
    unsigned samples = 0, pixelDifference = 0;
    const unsigned dx = std::max(1u, w / 80), dy = std::max(1u, h / 45);
    for (unsigned y = dy / 2; y < h; y += dy) {
        for (unsigned x = dx / 2; x < w; x += dx) {
            const size_t offset = (size_t(y) * w + x) * 4;
            for (unsigned c = 0; c < 3; ++c) {
                ++histogramDelta[c * 16 + (a[offset + c] >> 4)];
                --histogramDelta[c * 16 + (b[offset + c] >> 4)];
                pixelDifference += unsigned(std::abs(int(a[offset + c]) - int(b[offset + c])));
            }
            ++samples;
        }
    }
    unsigned difference = 0;
    for (int d : histogramDelta) difference += unsigned(std::abs(d));
    return difference > samples * 6 * 0.48 && pixelDifference > samples * 3 * 25;
}

#ifdef LIVE_NVIDIA_FLOW
#include "live_flow_quality.h"
#include <cuda.h>
#include <dlfcn.h>
#include <nvOpticalFlowCuda.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

// Resolve CUDA on first use so the ordinary X11 viewer can still run on a host
// without an installed NVIDIA CUDA driver, even if this binary includes FG.
#define FG_CUDA_FUNCTIONS(F) \
    F(cuInit) F(cuGetErrorString) F(cuDeviceGet) F(cuDevicePrimaryCtxRetain) F(cuDevicePrimaryCtxRelease) \
    F(cuCtxPushCurrent) F(cuCtxPopCurrent) F(cuStreamCreate) F(cuStreamDestroy) F(cuStreamSynchronize) \
    F(cuMemAlloc) F(cuMemFree) F(cuMemsetD32Async) F(cuMemcpy2D) F(cuMemcpyDtoH) F(cuModuleLoad) F(cuModuleUnload) \
    F(cuModuleGetFunction) F(cuLaunchKernel)

struct CudaDriver {
    std::unique_ptr<void, int (*)(void*)> library{dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL), dlclose};
#define FG_DECLARE(name) decltype(&::name) name{};
    FG_CUDA_FUNCTIONS(FG_DECLARE)
#undef FG_DECLARE

    CudaDriver() {
        if (!library) throw std::runtime_error("open NVIDIA CUDA driver: " + std::string(dlerror()));
        // cuda.h aliases driver functions to their ABI-specific names (e.g. cuMemAlloc_v2).
#define FG_SYMBOL_NAME(name) #name
#define FG_LOAD(name) \
        name = reinterpret_cast<decltype(name)>(dlsym(library.get(), FG_SYMBOL_NAME(name))); \
        if (!name) throw std::runtime_error("missing NVIDIA CUDA symbol " FG_SYMBOL_NAME(name));
        FG_CUDA_FUNCTIONS(FG_LOAD)
#undef FG_LOAD
#undef FG_SYMBOL_NAME
    }
};
#undef FG_CUDA_FUNCTIONS

const CudaDriver& cuda() {
    static const CudaDriver driver;
    return driver;
}

void checkCuda(CUresult result, const char* op) {
    if (result == CUDA_SUCCESS) return;
    const char* reason = nullptr;
    cuda().cuGetErrorString(result, &reason);
    throw std::runtime_error(std::string(op) + ": " + (reason ? reason : "CUDA driver error"));
}

struct CurrentContext {
    explicit CurrentContext(CUcontext context) { checkCuda(cuda().cuCtxPushCurrent(context), "activate optical-flow context"); }
    ~CurrentContext() { CUcontext popped; cuda().cuCtxPopCurrent(&popped); }
};

struct LumaLevels {
    double sum = 0, squares = 0;

    void add(double luma) {
        sum += luma;
        squares += luma * luma;
    }
    double contrast(unsigned samples) const {
        double mean = sum / samples;
        return std::sqrt(std::max(0.0, squares / samples - mean * mean));
    }
};

// Check the uncorrected warp as well as the displayed midpoint. Widespread
// overlay votes at a scene cut must not conceal a collapse in warp contrast.
using ExposureSample = std::array<uint8_t, 4>; // Previous, current, warped, displayed luma.
bool stableExposure(std::span<const ExposureSample> samples) {
    LumaLevels levels[4]{};
    for (const auto& sample : samples)
        for (unsigned i = 0; i < 4; ++i) levels[i].add(sample[i]);
    const unsigned count = unsigned(samples.size());
    const double exposureMargin = 10.0 * count; // SDR code values, summed over the samples
    const double low = std::min(levels[0].sum, levels[1].sum) - exposureMargin;
    const double high = std::max(levels[0].sum, levels[1].sum) + exposureMargin;
    const double contrast = std::min(levels[0].contrast(count), levels[1].contrast(count));
    const double minContrast = contrast - std::max(12.0, contrast * 0.22);
    for (unsigned i : {2u, 3u})
        if (levels[i].sum < low || levels[i].sum > high || levels[i].contrast(count) < minContrast) return false;
    return true;
}

struct FlowBuffer {
    NvOFGPUBufferHandle handle{};
    CUdeviceptr pointer{};
    unsigned pitch{};
};

bool unreliableMotion(const LiveFlowQuality& quality, bool holding) {
    unsigned probes = 0, changed = 0, fractured = 0, badRegions = 0;
    for (const auto& row : quality.regions) {
        for (const auto& region : row) {
            probes += region.probes;
            changed += region.changed;
            fractured += region.fractured;
            if (region.fractured >= 3 && region.fractured > .10 * region.probes &&
                region.fractured > region.changed * (holding ? .34 : .375))
                ++badRegions;
        }
    }
    // A moving foreground can occupy a small fraction of a static capture.
    // Look for clusters, while allowing an isolated occlusion at an object edge.
    const auto worstPhase = std::ranges::max(quality.unreliable);
    return (badRegions >= 3 && fractured > .105 * changed) || worstPhase > probes * (holding ? .14 : .18);
}

} // namespace

struct LiveFrameGen::Impl {
    unsigned w, h, phases;
    void* driver{};
    NV_OF_CUDA_API_FUNCTION_LIST api{};
    CUdevice device{};
    CUcontext context{};
    CUstream stream{};
    NvOFHandle session{};
    FlowBuffer previous, current, forward, backward;
    CUdeviceptr output{};
    std::vector<ExposureSample> exposure;
    bool holdingForMotion = false;
    CUmodule module{};
    CUfunction findOverlays{}, warp{};

    Impl(unsigned width, unsigned height, unsigned multiplier) : w(width), h(height), phases(multiplier - 1) {
        if (multiplier < 2 || multiplier > 4) throw std::invalid_argument("frame generation multiplier must be 2, 3 or 4");
    }
    ~Impl() {
        if (context) {
            if (cuda().cuCtxPushCurrent(context) == CUDA_SUCCESS) {
                if (stream) cuda().cuStreamSynchronize(stream);
                for (FlowBuffer* b : {&previous, &current, &forward, &backward})
                    if (b->handle) api.nvOFDestroyGPUBufferCuda(b->handle);
                if (session) api.nvOFDestroy(session);
                if (output) cuda().cuMemFree(output);
                if (module) cuda().cuModuleUnload(module);
                if (stream) cuda().cuStreamDestroy(stream);
                CUcontext popped;
                cuda().cuCtxPopCurrent(&popped);
            }
            cuda().cuDevicePrimaryCtxRelease(device);
        }
        if (driver) dlclose(driver);
    }

    void checkFlow(NV_OF_STATUS status, const char* op) const {
        if (status == NV_OF_SUCCESS) return;
        char message[512]{};
        uint32_t length = sizeof(message);
        if (session) api.nvOFGetLastError(session, message, &length);
        throw std::runtime_error(std::string(op) + " (NVIDIA optical flow " + std::to_string(status)
                                 + ")" + (*message ? std::string(": ") + message : ""));
    }

    void allocate(FlowBuffer& b, bool isFlow) {
        NV_OF_BUFFER_DESCRIPTOR desc{};
        desc.width = isFlow ? (w + 3) / 4 : w;
        desc.height = isFlow ? (h + 3) / 4 : h;
        desc.bufferFormat = isFlow ? NV_OF_BUFFER_FORMAT_SHORT2 : NV_OF_BUFFER_FORMAT_ABGR8;
        desc.bufferUsage = isFlow ? NV_OF_BUFFER_USAGE_OUTPUT : NV_OF_BUFFER_USAGE_INPUT;
        checkFlow(api.nvOFCreateGPUBufferCuda(session, &desc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &b.handle),
                  "allocate optical-flow buffer");
        NV_OF_CUDA_BUFFER_STRIDE_INFO strides{};
        checkFlow(api.nvOFGPUBufferGetStrideInfo(b.handle, &strides), "read optical-flow stride");
        b.pitch = strides.strideInfo[0].strideXInBytes;
        b.pointer = api.nvOFGPUBufferGetCUdeviceptr(b.handle);
        if (!b.pointer || b.pitch < size_t(desc.width) * 4)
            throw std::runtime_error("invalid optical-flow device buffer");
    }

    void init() {
        driver = dlopen("libnvidia-opticalflow.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!driver) throw std::runtime_error("open NVIDIA optical-flow driver: " + std::string(dlerror()));
        auto create = reinterpret_cast<decltype(&NvOFAPICreateInstanceCuda)>(dlsym(driver, "NvOFAPICreateInstanceCuda"));
        if (!create) throw std::runtime_error("NVIDIA driver lacks the CUDA optical-flow API");
        checkFlow(create(NV_OF_API_VERSION, &api), "open NVIDIA optical-flow API");
        checkCuda(cuda().cuInit(0), "initialize CUDA");
        checkCuda(cuda().cuDeviceGet(&device, 0), "select optical-flow GPU");
        checkCuda(cuda().cuDevicePrimaryCtxRetain(&context, device), "retain CUDA context");
        CurrentContext active(context);
        checkFlow(api.nvCreateOpticalFlowCuda(context, &session), "create optical-flow session");
        auto limit = [&](NV_OF_CAPS cap) {
            unsigned value = 0, length = 1;
            checkFlow(api.nvOFGetCaps(session, cap, &value, &length), "query optical-flow dimensions");
            return value;
        };
        const unsigned minW = limit(NV_OF_CAPS_WIDTH_MIN), maxW = limit(NV_OF_CAPS_WIDTH_MAX);
        const unsigned minH = limit(NV_OF_CAPS_HEIGHT_MIN), maxH = limit(NV_OF_CAPS_HEIGHT_MAX);
        if (w < minW || w > maxW || h < minH || h > maxH)
            throw std::runtime_error("optical-flow capture dimensions outside supported range "
                + std::to_string(minW) + "x" + std::to_string(minH) + " to "
                + std::to_string(maxW) + "x" + std::to_string(maxH));
        NV_OF_INIT_PARAMS p{};
        p.width = w; p.height = h;
        p.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        p.mode = NV_OF_MODE_OPTICALFLOW;
        p.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
        checkFlow(api.nvOFInit(session, &p), "initialize optical-flow session");
        checkCuda(cuda().cuStreamCreate(&stream, CU_STREAM_DEFAULT), "create optical-flow CUDA stream");
        checkFlow(api.nvOFSetIOCudaStreams(session, stream, stream), "set optical-flow streams");
        allocate(previous, false);
        allocate(current, false);
        allocate(forward, true);
        allocate(backward, true);
        const size_t tileCount = size_t((w + 3) / 4) * ((h + 3) / 4);
        exposure.resize(size_t((w + 15) / 16) * ((h + 15) / 16) * phases);
        checkCuda(cuda().cuMemAlloc(&output, size_t(w) * h * 4 * phases + sizeof(LiveFlowQuality) +
                                    exposure.size() * sizeof(ExposureSample) + tileCount),
                  "allocate generated frame, exposure samples and overlay mask");
        auto build = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path();
        auto path = build / "data/shaders/live_midpoint.ptx";
        checkCuda(cuda().cuModuleLoad(&module, path.c_str()), "load live_midpoint.ptx");
        checkCuda(cuda().cuModuleGetFunction(&findOverlays, module, "findOverlays"), "load overlay detection kernel");
        checkCuda(cuda().cuModuleGetFunction(&warp, module, "interpolateFrames"), "load interpolation kernel");
    }

    void upload(const uint8_t* host, const FlowBuffer& buffer) const {
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_HOST;
        copy.srcHost = host;
        copy.srcPitch = size_t(w) * 4;
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = buffer.pointer;
        copy.dstPitch = buffer.pitch;
        copy.WidthInBytes = copy.srcPitch;
        copy.Height = h;
        // Input pointers belong to the caller and can be pageable. A blocking stream
        // preserves ordering with the synchronous default-stream upload.
        checkCuda(cuda().cuMemcpy2D(&copy), "upload frame for optical flow");
    }

    void flow(const FlowBuffer& from, const FlowBuffer& to, const FlowBuffer& field) const {
        NV_OF_EXECUTE_INPUT_PARAMS in{};
        in.inputFrame = from.handle;
        in.referenceFrame = to.handle;
        in.disableTemporalHints = NV_OF_TRUE; // The two calls in each pair use opposite directions.
        NV_OF_EXECUTE_OUTPUT_PARAMS out{};
        out.outputBuffer = field.handle;
        checkFlow(api.nvOFExecute(session, &in, &out), "estimate NVIDIA optical flow");
    }

    bool interpolate(const uint8_t* a, const uint8_t* b, uint8_t* intermediates,
                     const uint8_t* sourceA, const uint8_t* sourceB) {
        const size_t bytes = size_t(w) * h * 4;
        if (!std::memcmp(sourceA, sourceB, bytes) || liveSceneCut(sourceA, sourceB, w, h)) return false;
        CurrentContext active(context);
        upload(sourceA, previous);
        upload(sourceB, current);
        flow(previous, current, forward);
        flow(current, previous, backward);
        const size_t batchBytes = bytes * phases;
        CUdeviceptr qualityDevice = output + batchBytes;
        CUdeviceptr samples = qualityDevice + sizeof(LiveFlowQuality);
        const size_t sampleBytes = exposure.size() * sizeof(ExposureSample);
        CUdeviceptr overlays = samples + sampleBytes;
        static_assert(sizeof(LiveFlowQuality) % sizeof(unsigned) == 0);
        checkCuda(cuda().cuMemsetD32Async(qualityDevice, 0, sizeof(LiveFlowQuality) / sizeof(unsigned), stream),
                  "clear motion quality counters");
        void* maskArgs[] = {&previous.pointer, &previous.pitch, &current.pointer, &current.pitch,
                            &forward.pointer, &forward.pitch, &backward.pointer, &backward.pitch,
                            &overlays, &w, &h, &qualityDevice};
        checkCuda(cuda().cuLaunchKernel(findOverlays, (w + 15) / 16, (h + 15) / 16, 1,
                                        16, 16, 1, 0, stream, maskArgs, nullptr), "identify screen-space overlays");
        if (sourceA != a || sourceB != b) {
            // Estimate motion on the captured scene. Changes made by NR should
            // alter the rendered colors without moving the correspondence field.
            checkCuda(cuda().cuStreamSynchronize(stream), "finish original-frame optical flow");
            upload(a, previous);
            upload(b, current);
        }
        void* args[] = {&previous.pointer, &previous.pitch, &current.pointer, &current.pitch,
                        &forward.pointer, &forward.pitch, &backward.pointer, &backward.pitch,
                        &output, &w, &h, &qualityDevice, &overlays, &samples};
        checkCuda(cuda().cuLaunchKernel(warp, (w + 15) / 16, (h + 15) / 16, phases, 16, 16, 1, 0, stream, args, nullptr),
                  "warp intermediate frames");
        checkCuda(cuda().cuStreamSynchronize(stream), "finish generated frame");
        LiveFlowQuality quality{};
        checkCuda(cuda().cuMemcpyDtoH(&quality, qualityDevice, sizeof(quality)), "read motion quality counters");
        const unsigned probes = ((w + 7) / 8) * ((h + 7) / 8);
        // Leave some headroom before resuming so borderline pairs do not
        // repeatedly switch between source cadence and synthetic frames.
        holdingForMotion = unreliableMotion(quality, holdingForMotion);
        if (holdingForMotion || std::ranges::max(quality.unmatched) > probes * .60) return false;
        checkCuda(cuda().cuMemcpyDtoH(exposure.data(), samples, sampleBytes), "read generated frame exposure");
        const size_t samplesPerFrame = exposure.size() / phases;
        for (size_t i = 0; i < phases; ++i) {
            if (!stableExposure(std::span(exposure).subspan(i * samplesPerFrame, samplesPerFrame))) return false;
        }
        checkCuda(cuda().cuMemcpyDtoH(intermediates, output, batchBytes), "read generated frames");
        return true;
    }
};

LiveFrameGen::LiveFrameGen(unsigned w, unsigned h, unsigned multiplier) : impl_(std::make_unique<Impl>(w, h, multiplier)) { impl_->init(); }

bool LiveFrameGen::interpolate(const uint8_t* previous, const uint8_t* current, uint8_t* intermediates,
                               const uint8_t* previousSource, const uint8_t* currentSource) {
    return impl_->interpolate(previous, current, intermediates, previousSource ? previousSource : previous,
                              currentSource ? currentSource : current);
}
#else
struct LiveFrameGen::Impl {};
LiveFrameGen::LiveFrameGen(unsigned, unsigned, unsigned) {
    throw std::runtime_error("frame generation requires a build with the CUDA Toolkit and nvcc");
}
bool LiveFrameGen::interpolate(const uint8_t*, const uint8_t*, uint8_t*, const uint8_t*, const uint8_t*) {
    return false;
}
#endif

LiveFrameGen::~LiveFrameGen() = default;
