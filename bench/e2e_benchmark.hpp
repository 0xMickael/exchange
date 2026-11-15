#pragma once

#include "clock.hpp"
#include "engine_benchmark.hpp"
#include "environment.hpp"
#include "workload.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace exchange::bench {

// The server configurations the benchmark can measure.
enum class ServerKind : std::uint8_t {
    Inline,          // One thread does everything.
    PipelineBackoff, // Engine on its own thread; it naps when idle.
    PipelineSpin,    // Engine on its own thread; it never sleeps.
};

[[nodiscard]] std::string to_string(ServerKind kind);

struct E2eConfig {
    // Uses seed, initial_orders, price_levels, cancel_percent,
    // aggressive_percent, mid_price and max_quantity. `initial_orders` is the
    // total for the book, shared between the connections.
    WorkloadConfig workload;
    std::vector<ServerKind> servers = {ServerKind::Inline, ServerKind::PipelineBackoff,
                                       ServerKind::PipelineSpin};
    std::size_t connections = 1;
    // Measured requests sent by each connection.
    std::size_t requests = 100'000;
    // Requests per second sent by each connection. 0 means closed loop: a
    // connection sends its next request when the previous one is answered.
    double rate = 0.0;
    std::uint32_t runs = 3;
    ClockChoice clock = ClockChoice::Automatic;
};

struct E2eServerResult {
    ServerKind kind = ServerKind::Inline;
    // Latencies are per request; throughput is requests per second over all connections.
    std::vector<EngineRunResult> runs;
    // Cancels that reached the server after their order had been filled.
    std::uint64_t cancel_rejects = 0;
};

struct E2eResult {
    ClockDescription clock;
    std::vector<E2eServerResult> servers;
    // Empty if the measurement is trustworthy, otherwise what went wrong.
    std::string error;
};

// Measures requests sent over TCP to a server running in this process, on the
// loopback interface, following SPEC section 13.6.
[[nodiscard]] E2eResult run_e2e_benchmark(const E2eConfig& config);

void print_e2e_report(std::ostream& out, const Environment& environment, const E2eConfig& config,
                      const E2eResult& result);
void print_e2e_json(std::ostream& out, const Environment& environment, const E2eConfig& config,
                    const E2eResult& result);

} // namespace exchange::bench
