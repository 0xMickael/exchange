#pragma once

#include "environment.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace exchange::bench {

struct QueueBenchmarkConfig {
    // Messages sent from the producer thread to the consumer thread in each run.
    std::size_t messages = 10'000'000;
    std::size_t capacity = 4096;
    std::uint32_t runs = 5;
};

struct QueueBenchmarkResult {
    std::size_t message_size = 0;
    // One entry per run, in messages per second.
    std::vector<double> spsc_throughput;
    std::vector<double> mutex_throughput;
    // Empty if every message arrived in order, otherwise what went wrong.
    std::string error;
};

// Sends the same stream of commands through SpscQueue and through MutexQueue,
// with one producer thread and one consumer thread, and measures how many
// messages per second each queue transfers.
[[nodiscard]] QueueBenchmarkResult run_queue_benchmark(const QueueBenchmarkConfig& config);

void print_queue_report(std::ostream& out, const Environment& environment,
                        const QueueBenchmarkConfig& config, const QueueBenchmarkResult& result);

void print_queue_json(std::ostream& out, const Environment& environment,
                      const QueueBenchmarkConfig& config, const QueueBenchmarkResult& result);

} // namespace exchange::bench
