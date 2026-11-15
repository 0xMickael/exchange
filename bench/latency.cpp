#include "latency.hpp"

#include <algorithm>
#include <cassert>
#include <numeric>

namespace exchange::bench {

std::uint64_t percentile(std::span<const std::uint64_t> sorted, std::uint32_t per_ten_thousand) {
    assert(!sorted.empty());
    assert(per_ten_thousand <= 10'000);
    // rank = ceil(count * share), counted from 1.
    const std::uint64_t rank = ((sorted.size() * per_ten_thousand) + 9'999) / 10'000;
    const std::uint64_t index = std::clamp<std::uint64_t>(rank, 1, sorted.size()) - 1;
    return sorted[index];
}

LatencySummary summarize(std::vector<std::uint64_t>& samples) {
    LatencySummary summary;
    if (samples.empty()) {
        return summary;
    }
    std::ranges::sort(samples);
    summary.count = samples.size();
    summary.min = samples.front();
    summary.p50 = percentile(samples, 5'000);
    summary.p95 = percentile(samples, 9'500);
    summary.p99 = percentile(samples, 9'900);
    summary.p999 = percentile(samples, 9'990);
    summary.max = samples.back();
    const std::uint64_t sum = std::accumulate(samples.begin(), samples.end(), std::uint64_t{0});
    summary.mean = static_cast<double>(sum) / static_cast<double>(samples.size());
    return summary;
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    return values[(values.size() - 1) / 2];
}

} // namespace exchange::bench
