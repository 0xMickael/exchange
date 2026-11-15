#pragma once

#include "clock.hpp"
#include "latency.hpp"
#include "workload.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace exchange::bench {

struct EngineBenchmarkConfig {
    WorkloadConfig workload;
    // Measured repetitions of the whole workload.
    std::uint32_t runs = 5;
    // Unmeasured repetitions executed first.
    std::uint32_t warmup_runs = 1;
    ClockChoice clock = ClockChoice::Automatic;
};

// The clock a benchmark used, and what it can and cannot resolve.
struct ClockDescription {
    std::string name;
    double ticks_per_nanosecond = 0.0;
    double smallest_interval_ns = 0.0;
    double reading_cost_ns = 0.0;
};

struct EngineRunResult {
    LatencySummary latency_ns;
    double commands_per_second = 0.0;
};

struct EngineBenchmarkResult {
    WorkloadStats workload;
    ClockDescription clock;
    std::vector<EngineRunResult> runs;
    // Empty if the measurement is trustworthy, otherwise what went wrong.
    std::string error;
};

// Measures MatchingEngine::process() alone, on the calling thread, following
// the method of SPEC section 13.
[[nodiscard]] EngineBenchmarkResult run_engine_benchmark(const EngineBenchmarkConfig& config);

} // namespace exchange::bench
