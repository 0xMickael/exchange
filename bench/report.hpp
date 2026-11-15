#pragma once

#include "engine_benchmark.hpp"
#include "environment.hpp"

#include <iosfwd>

namespace exchange::bench {

// A human-readable report of an engine benchmark.
void print_engine_report(std::ostream& out, const Environment& environment,
                         const EngineBenchmarkConfig& config, const EngineBenchmarkResult& result);

// The same content as JSON, for scripts and for keeping results over time.
void print_engine_json(std::ostream& out, const Environment& environment,
                       const EngineBenchmarkConfig& config, const EngineBenchmarkResult& result);

} // namespace exchange::bench
