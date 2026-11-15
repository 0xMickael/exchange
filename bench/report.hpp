#pragma once

#include "engine_benchmark.hpp"
#include "environment.hpp"

#include <iosfwd>
#include <vector>

namespace exchange::bench {

// A run whose every statistic is the median of that statistic over the given runs.
[[nodiscard]] EngineRunResult median_run(const std::vector<EngineRunResult>& runs);

// A human-readable report of an engine benchmark.
void print_engine_report(std::ostream& out, const Environment& environment,
                         const EngineBenchmarkConfig& config, const EngineBenchmarkResult& result);

// The same content as JSON, for scripts and for keeping results over time.
void print_engine_json(std::ostream& out, const Environment& environment,
                       const EngineBenchmarkConfig& config, const EngineBenchmarkResult& result);

} // namespace exchange::bench
