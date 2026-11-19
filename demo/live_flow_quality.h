#pragma once

// Shared layout for the CUDA kernels and the host readback.
struct LiveFlowQuality {
    static constexpr unsigned columns = 8, rows = 6;
    struct Region {
        unsigned probes, changed, fractured;
    };
    // Per-phase counters share one bidirectional field and region table.
    unsigned unmatched[3], unreliable[3];
    Region regions[rows][columns];
};
