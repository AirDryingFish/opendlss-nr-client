#pragma once
#include <array>
#include <cstdint>
#include <memory>

class MakoExchange {
public:
    MakoExchange(unsigned width, unsigned height, unsigned outputCount);
    ~MakoExchange();
    std::array<uint8_t, 16> deviceUuid() const;
    bool supportsHalfStorage() const;
    int exportImage(unsigned index) const;
    int exportTimeline() const;
    // An overwritten source may discard its previous content and ownership. On the first upload
    // initialize both images before the runtime's first acquisition of the pair.
    void upload(unsigned slot, const uint8_t* pixels, uint64_t signal, bool first);
    void download(uint8_t* pixels, uint64_t wait);
    bool lastUploadMatches(const uint8_t* pixels) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
