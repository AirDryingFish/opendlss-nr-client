// The NVIDIA optical-flow engine supplies a bidirectional S10.5 displacement on a 4x4 grid.
// Invert each fractional flow at the destination and sample both original exposures.
#include <cuda_runtime.h>
#include "../live_flow_quality.h"

template<class T>
__device__ const T& pixel(const char* image, unsigned pitch, int x, int y) {
    return reinterpret_cast<const T*>(image + size_t(y) * pitch)[x];
}

__device__ float2 lerp2(float2 a, float2 b, float t) {
    return make_float2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

__device__ float4 lerp4(float4 a, float4 b, float t) {
    return make_float4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                       a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

__device__ float2 unpack(short2 v) {
    return make_float2(v.x / 32.f, v.y / 32.f);
}

__device__ float4 unpack(uchar4 v) {
    return make_float4(v.x, v.y, v.z, v.w);
}

struct Raster {
    const char* data;
    unsigned pitch;
    int width, height;
};

__device__ float2 motion(Raster field, float2 pos) {
    // Sample displacements at their block centers.
    float x = fminf(fmaxf((pos.x - 1.5f) * .25f, 0.f), field.width - 1.f);
    float y = fminf(fmaxf((pos.y - 1.5f) * .25f, 0.f), field.height - 1.f);
    int x0 = int(x), x1 = min(x0 + 1, field.width - 1);
    int y0 = int(y), y1 = min(y0 + 1, field.height - 1);
    float2 top = lerp2(unpack(pixel<short2>(field.data, field.pitch, x0, y0)),
                       unpack(pixel<short2>(field.data, field.pitch, x1, y0)), x - x0);
    float2 bottom = lerp2(unpack(pixel<short2>(field.data, field.pitch, x0, y1)),
                          unpack(pixel<short2>(field.data, field.pitch, x1, y1)), x - x0);
    return lerp2(top, bottom, y - y0);
}

__device__ float4 color(Raster image, float2 pos) {
    float x = fminf(fmaxf(pos.x, 0.f), image.width - 1.f);
    float y = fminf(fmaxf(pos.y, 0.f), image.height - 1.f);
    int x0 = int(x), x1 = min(x0 + 1, image.width - 1);
    int y0 = int(y), y1 = min(y0 + 1, image.height - 1);
    float4 top = lerp4(unpack(pixel<uchar4>(image.data, image.pitch, x0, y0)),
                       unpack(pixel<uchar4>(image.data, image.pitch, x1, y0)), x - x0);
    float4 bottom = lerp4(unpack(pixel<uchar4>(image.data, image.pitch, x0, y1)),
                          unpack(pixel<uchar4>(image.data, image.pitch, x1, y1)), x - x0);
    return lerp4(top, bottom, y - y0);
}

__device__ uchar4 texel(Raster image, int x, int y) {
    return pixel<uchar4>(image.data, image.pitch, min(max(x, 0), image.width - 1),
                         min(max(y, 0), image.height - 1));
}

__device__ int luma(uchar4 v) {
    return (54 * v.x + 183 * v.y + 19 * v.z) >> 8;
}

__device__ uchar4 pack(float4 v) {
    return make_uchar4(__float2uint_rn(v.x), __float2uint_rn(v.y), __float2uint_rn(v.z), __float2uint_rn(v.w));
}

__device__ float rgbDistance(float4 a, float4 b) {
    return fmaxf(fabsf(a.x - b.x), fmaxf(fabsf(a.y - b.y), fabsf(a.z - b.z)));
}

// A stroke is brighter or darker than its neighbors on both sides.
__device__ int strokeContrast(int first, int second) {
    return (first < 0) == (second < 0) ? min(abs(first), abs(second)) : 0;
}

// A letter anchored to the screen keeps an edge at the same position and
// with the same polarity, even when its background moves underneath it.
__device__ bool sameEdge(int contrast, int otherContrast) {
    return abs(contrast) > 50 && abs(otherContrast) > 50 &&
           (contrast < 0) == (otherContrast < 0) && abs(contrast - otherContrast) < 26;
}

// A newly appearing or disappearing cue has no matching screen-space edge
// yet. Require neighboring ink of the same color along its stroke. This
// keeps isolated bright or dark points in moving textures on the flow path.
__device__ bool hasNeighboringInk(Raster image, int x, int y, float4 ink) {
    unsigned count = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if ((dx || dy) && rgbDistance(ink, unpack(texel(image, x + dx, y + dy))) < 7.f) {
                if (++count == 2) return true;
            }
        }
    }
    return false;
}

__device__ bool overlayStroke(Raster from, Raster to, int x, int y, uchar4 here,
                              uchar4 there, float2 flow) {
    int center = luma(here);
    int left = center - luma(texel(from, x - 2, y));
    int right = center - luma(texel(from, x + 2, y));
    int top = center - luma(texel(from, x, y - 2));
    int bottom = center - luma(texel(from, x, y + 2));
    int edge = max(max(abs(left), abs(right)), max(abs(top), abs(bottom)));
    if (edge < 36) return false;
    int ridge = max(strokeContrast(left, right), strokeContrast(top, bottom));
    int other = luma(there);
    bool stable = rgbDistance(unpack(here), unpack(there)) <= 12.f &&
            (sameEdge(left, other - luma(texel(to, x - 2, y))) ||
             sameEdge(right, other - luma(texel(to, x + 2, y))) ||
             sameEdge(top, other - luma(texel(to, x, y - 2))) ||
             sameEdge(bottom, other - luma(texel(to, x, y + 2))));
    if (!stable && (ridge < 48 || !hasNeighboringInk(from, x, y, unpack(here)))) return false;
    if (stable && flow.x * flow.x + flow.y * flow.y < 1.f) return ridge >= 32;
    float4 predicted = color(to, make_float2(x + flow.x, y + flow.y));
    return rgbDistance(unpack(here), predicted) >= 28.f;
}

enum : unsigned { PreviousOverlay = 1, CurrentOverlay = 2 };

// Check motion on the original captures, before NR changes their colors.
// A discontinuity is normal at a silhouette. A discontinuity whose reverse
// track also misses is evidence that neighboring blocks found different surfaces.
__device__ void probeMotion(Raster f, Raster b, float2 pos, LiveFlowQuality::Region& region,
                            uchar4 previous, uchar4 current) {
    atomicAdd(&region.probes, 1u);
    if (rgbDistance(unpack(previous), unpack(current)) <= 15.f) return;
    atomicAdd(&region.changed, 1u);
    float2 d = motion(f, pos);
    float2 right = motion(f, make_float2(pos.x + 4.f, pos.y));
    float2 below = motion(f, make_float2(pos.x, pos.y + 4.f));
    if (fmaxf(hypotf(d.x - right.x, d.y - right.y), hypotf(d.x - below.x, d.y - below.y)) <= 3.f) return;
    float2 reverse = motion(b, make_float2(pos.x + d.x, pos.y + d.y));
    if (hypotf(d.x + reverse.x, d.y + reverse.y) > 2.f + .1f * hypotf(d.x, d.y))
        atomicAdd(&region.fractured, 1u);
}

// Each mask cell covers one 4x4 flow block. Both endpoints vote, so a new or
// disappearing stroke can be held at source cadence for the subtitle change.
extern "C" __global__ void findOverlays(const char* previous, unsigned previousPitch,
    const char* current, unsigned currentPitch, const char* forward, unsigned forwardPitch,
    const char* backward, unsigned backwardPitch, unsigned char* mask, int width, int height,
    LiveFlowQuality* quality) {
    int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
    Raster a{previous, previousPitch, width, height}, b{current, currentPitch, width, height};
    __shared__ unsigned char votes[16][16];
    unsigned vote = 0;
    if (x < width && y < height) {
        uchar4 prev = texel(a, x, y), curr = texel(b, x, y);
        if (!(x & 7) && !(y & 7)) {
            Raster first{forward, forwardPitch, (width + 3) / 4, (height + 3) / 4};
            Raster second{backward, backwardPitch, first.width, first.height};
            auto& region = quality->regions[y * LiveFlowQuality::rows / height][x * LiveFlowQuality::columns / width];
            probeMotion(first, second, make_float2(x, y), region, prev, curr);
        }
        float2 f = unpack(pixel<short2>(forward, forwardPitch, x / 4, y / 4));
        float2 r = unpack(pixel<short2>(backward, backwardPitch, x / 4, y / 4));
        if (overlayStroke(a, b, x, y, prev, curr, f)) vote |= PreviousOverlay;
        if (overlayStroke(b, a, x, y, curr, prev, r)) vote |= CurrentOverlay;
    }
    votes[threadIdx.y][threadIdx.x] = vote;
    __syncthreads();
    if (x >= width || y >= height || (x & 3) || (y & 3)) return;
    unsigned count = 0, endpoints = 0;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            unsigned bits = votes[threadIdx.y + j][threadIdx.x + i];
            count += bits != 0;
            endpoints |= bits;
        }
    }
    mask[(y / 4) * ((width + 3) / 4) + x / 4] = count >= 2 ? endpoints : 0;
}

__device__ unsigned overlayAt(const unsigned char* mask, int width, int height, float2 pos) {
    // Cover the bilinear footprint and a one-pixel antialiasing fringe.
    int x0 = min(max(int(floorf(pos.x)) - 1, 0), width - 1) / 4;
    int x1 = min(max(int(ceilf(pos.x)) + 1, 0), width - 1) / 4;
    int y0 = min(max(int(floorf(pos.y)) - 1, 0), height - 1) / 4;
    int y1 = min(max(int(ceilf(pos.y)) + 1, 0), height - 1) / 4;
    int stride = (width + 3) / 4;
    return mask[y0 * stride + x0] | mask[y0 * stride + x1] | mask[y1 * stride + x0] | mask[y1 * stride + x1];
}

__device__ float4 warp(Raster image, Raster other, Raster flow, Raster opposite, float2 mid, float fraction,
                       float& confidence, float2& p) {
    p = mid;
    for (int k = 0; k < 2; ++k) {
        float2 d = motion(flow, p);
        p = make_float2(mid.x - fraction * d.x, mid.y - fraction * d.y);
    }
    float2 d = motion(flow, p);
    float2 end = make_float2(p.x + d.x, p.y + d.y);
    float2 back = motion(opposite, end);
    float dx = d.x + back.x, dy = d.y + back.y;
    // A hit outside the source, or a flow with no matching reverse, is likely an occlusion.
    bool inside = p.x >= 0 && p.y >= 0 && p.x < image.width && p.y < image.height;
    float4 sample = color(image, p);
    // At a moving silhouette the two vectors can cancel even when this track
    // lands on a different surface. Verify the landing color and midpoint too.
    float error = rgbDistance(sample, color(other, end)) / 16.f;
    float mx = p.x + fraction * d.x - mid.x, my = p.y + fraction * d.y - mid.y;
    confidence = (inside ? 1.f : .01f) / ((1.f + dx * dx + dy * dy) * (1.f + mx * mx + my * my + error * error));
    return sample;
}

__device__ float limitChannel(float v, float first, float second) {
    return fminf(fmaxf(v, fminf(first, second) - 2.f), fmaxf(first, second) + 2.f);
}

extern "C" __global__ void interpolateFrames(const char* previous, unsigned previousPitch,
    const char* current, unsigned currentPitch, const char* forward, unsigned forwardPitch,
    const char* backward, unsigned backwardPitch, uchar4* output, int width, int height, LiveFlowQuality* quality,
    const unsigned char* overlays, uchar4* exposure) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    Raster f{forward, forwardPitch, (width + 3) / 4, (height + 3) / 4};
    Raster b{backward, backwardPitch, f.width, f.height};
    const unsigned phase = blockIdx.z;
    const float t = float(phase + 1) / float(gridDim.z + 1);
    float2 mid = make_float2(x, y);
    float aw, bw;
    float2 pa, pb;
    Raster first{previous, previousPitch, width, height}, second{current, currentPitch, width, height};
    float4 a = warp(first, second, f, b, mid, t, aw, pa);
    float4 c = warp(second, first, b, f, mid, 1.f - t, bw, pb);
    float colorError = rgbDistance(a, c);
    float4 oldColor = unpack(texel(first, x, y)), newColor = unpack(texel(second, x, y));
    if (!(x & 7) && !(y & 7)) {
        // A cut can preserve the histogram. Rapid occlusion can instead
        // leave both tracks unreliable across large parts of the picture.
        if (fabsf(a.x-c.x) + fabsf(a.y-c.y) + fabsf(a.z-c.z) > 144.f)
            atomicAdd(&quality->unmatched[phase], 1u);
        if (aw + bw < .45f && colorError > 28.f && rgbDistance(oldColor, newColor) + 14.f < colorError)
            atomicAdd(&quality->unreliable[phase], 1u);
    }
    // Hold screen-space strokes at their original positions. Also exclude
    // those strokes from samples pulled into the surrounding background.
    unsigned maskA = overlayAt(overlays, width, height, pa);
    bool rejectA = maskA != 0;
    bool rejectB = overlayAt(overlays, width, height, pb) != 0;
    unsigned here = overlayAt(overlays, width, height, mid);
    float4 warped = lerp4(a, c, t * bw / ((1.f - t) * aw + t * bw));
    float4 v = warped;
    if (here || (rejectA && rejectB)) {
        // Small exposure changes (including NR) can interpolate at fixed screen
        // coordinates. For a cue change, keep the old text until the real frame.
        if (rgbDistance(oldColor, newColor) <= 32.f) v = lerp4(oldColor, newColor, t);
        else if (here == CurrentOverlay && !(maskA & PreviousOverlay)) v = a; // Newly covered moving scenery.
        else v = oldColor;
    } else if (rejectA || rejectB) {
        v = rejectA ? c : a;
    }
    // An unreliable warp can briefly introduce a highlight or dark patch
    // missing from both exposures. Retain such excursions for well-tracked motion.
    float disagreement = colorError / 20.f;
    float trust = fminf(1.f, .8f * (aw + bw)) / (1.f + disagreement * disagreement);
    float4 bounded = make_float4(limitChannel(v.x, oldColor.x, newColor.x),
                                 limitChannel(v.y, oldColor.y, newColor.y),
                                 limitChannel(v.z, oldColor.z, newColor.z), v.w);
    v = lerp4(bounded, v, trust);
    uchar4 displayed = pack(v);
    output[(size_t(phase) * height + y) * width + x] = displayed;
    // Sample one position per block for the whole-frame quality check. Vary
    // its location so repeating lines cannot alias all the exposure samples.
    unsigned block = blockIdx.y * gridDim.x + blockIdx.x;
    unsigned hash = (block + 1u) * 2654435761u;
    int sampleX = min(int((hash >> 16) & 15), width  - 1 - int(blockIdx.x * blockDim.x));
    int sampleY = min(int((hash >> 24) & 15), height - 1 - int(blockIdx.y * blockDim.y));
    if (int(threadIdx.x) == sampleX && int(threadIdx.y) == sampleY) {
        exposure[size_t(phase) * gridDim.x * gridDim.y + block] = make_uchar4(luma(pixel<uchar4>(previous, previousPitch, x, y)),
                                      luma(pixel<uchar4>(current, currentPitch, x, y)),
                                      luma(pack(warped)), luma(displayed));
    }
}
