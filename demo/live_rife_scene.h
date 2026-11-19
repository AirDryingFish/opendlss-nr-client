#pragma once

#include <cstddef>
#include <cstdint>

// Practical-RIFE skips synthesis below 0.2 SSIM on a 32x32 thumbnail.
// For live capture, pool RGB statistics in 8x8 windows and sample the same
// point in each cell of both frames. Varying its position across cells keeps
// thin repeating patterns from aliasing to a flat, apparently identical pair.
inline bool rifeSceneCut(const uint8_t* a, const uint8_t* b, unsigned width, unsigned height) {
    constexpr unsigned side = 32, window = 8, count = window * window;
    double totalSimilarity = 0;
    for (unsigned top = 0; top < side; top += window) {
        for (unsigned left = 0; left < side; left += window) {
            unsigned sumA[3]{}, sumB[3]{}, squares = 0, products = 0;
            for (unsigned y = top; y < top + window; ++y) {
                for (unsigned x = left; x < left + window; ++x) {
                    const uint32_t hash = (y * side + x + 1) * 2654435761u;
                    const auto sx = uint64_t(x * 256 + ((hash >> 16) & 255)) * width / (side * 256);
                    const auto sy = uint64_t(y * 256 + (hash >> 24)) * height / (side * 256);
                    const size_t offset = (sy * width + sx) * 4;
                    for (unsigned c = 0; c < 3; ++c) {
                        const unsigned av = a[offset + c], bv = b[offset + c];
                        sumA[c] += av;
                        sumB[c] += bv;
                        squares += av * av + bv * bv;
                        products += av * bv;
                    }
                }
            }
            double meanSquares = 0, meanProduct = 0;
            for (unsigned c = 0; c < 3; ++c) {
                const double ma = double(sumA[c]) / count, mb = double(sumB[c]) / count;
                meanSquares += ma * ma + mb * mb;
                meanProduct += ma * mb;
            }
            // SSIM stability constants for byte samples, summed over R, G and B.
            constexpr double c1 = 3 * 2.55 * 2.55, c2 = 3 * 7.65 * 7.65;
            const double variance = double(squares) / count - meanSquares;
            const double covariance = double(products) / count - meanProduct;
            totalSimilarity += ((2 * meanProduct + c1) / (meanSquares + c1)) *
                               ((2 * covariance + c2) / (variance + c2));
        }
    }
    constexpr unsigned windows = (side / window) * (side / window);
    return totalSimilarity < .2 * windows;
}
