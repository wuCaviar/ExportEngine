#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ATHC::EE {

// ============================================================================
//  Pre-decode wave scheduler (pure function)
// ============================================================================
//  Partitions decode work into sequential "waves" such that each wave's
//  total cost stays within `budget` and holds at most `maxParallel` items.
//  An item whose cost alone exceeds the budget runs in its own single-item
//  wave (serial — peak memory never exceeds that one decode). Items are
//  assigned in input order; wave k finishes entirely before wave k+1 starts,
//  preserving LRU-friendly ordering between waves.
//
//  cost unit: estimated transient bytes of a decode (see estimateDecodeCost
//  below and probeDecodeCost in SceneRenderer.cpp).

// ============================================================================
//  Pre-decode cost model (pure function)
// ============================================================================
//  Estimated transient peak bytes of one decode item. Image sources decode
//  lazily at render time (DecodedSource streams per band — no full frame is
//  ever materialized), so the only transient is the source-resolution decode
//  (raw + CMYK) with a 1.5× safety factor. Pre-decode itself is now a cheap
//  header/transform probe.
inline uint64_t estimateDecodeCost(uint64_t srcW, uint64_t srcH, uint64_t chans)
{
    const uint64_t cost = srcW * srcH * chans * 3 / 2; // decode transient (×1.5)
    return cost == 0 ? 1 : cost;
}
inline std::vector<std::vector<size_t>>
schedulePredecodeWaves(const std::vector<uint64_t> &costs, uint64_t budget, size_t maxParallel)
{
    std::vector<std::vector<size_t>> waves;
    if (maxParallel == 0 || costs.empty())
        return waves;

    size_t i = 0;
    while (i < costs.size()) {
        std::vector<size_t> wave;
        uint64_t            used = 0;
        while (i < costs.size() && wave.size() < maxParallel) {
            const uint64_t c = costs[i];
            if (!wave.empty() && used + c > budget)
                break; // budget exhausted — start next wave
            wave.push_back(i);
            used += c;
            ++i;
        }
        waves.push_back(std::move(wave));
    }
    return waves;
}

} // namespace ATHC::EE
