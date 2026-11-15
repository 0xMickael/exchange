#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace exchange::bench {

struct LatencySummary {
    std::uint64_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
    std::uint64_t max = 0;
    double mean = 0.0;
};

// The value below which the given share of the samples fall, using the
// nearest-rank definition: the smallest sample such that at least that share
// of all samples are less than or equal to it. The share is given in units of
// 1/10000, so 9990 means the 99.9th percentile. This keeps the rank an exact
// integer computation.
//
// Precondition: `sorted` is not empty and is in increasing order.
[[nodiscard]] std::uint64_t percentile(std::span<const std::uint64_t> sorted,
                                       std::uint32_t per_ten_thousand);

// Sorts the samples in place and summarizes them. Every sample is kept, so the
// percentiles are exact, not estimated from a histogram.
[[nodiscard]] LatencySummary summarize(std::vector<std::uint64_t>& samples);

// The median of a set of values; for an even count, the lower of the two middle values.
[[nodiscard]] double median(std::vector<double> values);

} // namespace exchange::bench
