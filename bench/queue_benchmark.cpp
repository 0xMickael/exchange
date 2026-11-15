#include "queue_benchmark.hpp"

#include "latency.hpp"
#include "mutex_queue.hpp"

#include "exchange/core/command.hpp"
#include "exchange/queue/spsc_queue.hpp"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <optional>
#include <ostream>
#include <thread>

#ifdef __x86_64__
#include <immintrin.h>
#endif

namespace exchange::bench {

namespace {

// Tells the processor that this thread is in a short waiting loop.
inline void cpu_relax() noexcept {
#ifdef __x86_64__
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

// A command of the size the server will really queue, numbered by `sequence`.
Command numbered_command(std::uint64_t sequence) {
    return NewOrder{.session_id = 1,
                    .client_order_id = sequence,
                    .side = Side::Buy,
                    .price = 100,
                    .quantity = 1};
}

// Transfers `messages` commands between two threads and returns the
// throughput in messages per second, or nullopt if a command arrived out of
// order. Both threads spin while the queue is full or empty.
template <typename Queue>
std::optional<double> transfer(std::size_t capacity, std::size_t messages) {
    Queue queue(capacity);
    std::atomic<bool> start{false};

    std::jthread producer([&queue, &start, messages] {
        while (!start.load(std::memory_order_acquire)) {
            cpu_relax();
        }
        for (std::size_t i = 0; i < messages; ++i) {
            const Command command = numbered_command(i);
            while (!queue.try_push(command)) {
                cpu_relax();
            }
        }
    });

    bool in_order = true;
    Command command;
    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < messages; ++i) {
        while (!queue.try_pop(command)) {
            cpu_relax();
        }
        const auto* order = std::get_if<NewOrder>(&command);
        in_order = in_order && order != nullptr && order->client_order_id == i;
    }
    const auto end = std::chrono::steady_clock::now();
    producer.join();

    if (!in_order) {
        return std::nullopt;
    }
    const double seconds = std::chrono::duration<double>(end - begin).count();
    return seconds > 0.0 ? static_cast<double>(messages) / seconds : 0.0;
}

double ratio(const QueueBenchmarkResult& result) {
    const double mutex = median(result.mutex_throughput);
    return mutex > 0.0 ? median(result.spsc_throughput) / mutex : 0.0;
}

} // namespace

QueueBenchmarkResult run_queue_benchmark(const QueueBenchmarkConfig& config) {
    QueueBenchmarkResult result;
    result.message_size = sizeof(Command);

    for (std::uint32_t run = 0; run < config.runs; ++run) {
        const std::optional<double> spsc =
            transfer<SpscQueue<Command>>(config.capacity, config.messages);
        const std::optional<double> mutex =
            transfer<MutexQueue<Command>>(config.capacity, config.messages);
        if (!spsc.has_value() || !mutex.has_value()) {
            result.error = "a queue delivered messages out of order";
            return result;
        }
        result.spsc_throughput.push_back(*spsc);
        result.mutex_throughput.push_back(*mutex);
    }
    return result;
}

void print_queue_report(std::ostream& out, const Environment& environment,
                        const QueueBenchmarkConfig& config, const QueueBenchmarkResult& result) {
    out << "Queue benchmark: one producer thread sends commands to one consumer thread\n\n";

    out << "Environment\n"
        << "  compiler     " << environment.compiler << '\n'
        << "  build        " << environment.build_type << " (" << environment.compiler_flags
        << ")\n"
        << "  system       " << environment.operating_system
        << (environment.wsl ? " [WSL2: virtualized, timings are indicative]" : "") << '\n'
        << "  cpu          " << environment.cpu_model << ", " << environment.logical_cpus
        << " logical CPUs\n\n";

    out << "Method\n"
        << "  " << config.messages << " messages of " << result.message_size
        << " bytes per run, queue capacity " << config.capacity << ", " << config.runs
        << " run(s)\n"
        << "  both threads spin while the queue is full or empty, so each uses a whole core\n"
        << "  time is measured on the consumer, from the start signal to the last message\n\n";

    out << "Throughput in messages per second\n"
        << "  run        spsc_queue    mutex_queue\n"
        << std::fixed << std::setprecision(0);
    for (std::size_t i = 0; i < result.spsc_throughput.size(); ++i) {
        out << "  " << std::left << std::setw(7) << i + 1 << std::right << std::setw(14)
            << result.spsc_throughput[i] << std::setw(15) << result.mutex_throughput[i] << '\n';
    }
    out << "  " << std::left << std::setw(7) << "median" << std::right << std::setw(14)
        << median(result.spsc_throughput) << std::setw(15) << median(result.mutex_throughput)
        << "\n\n"
        << std::setprecision(1) << "  spsc_queue / mutex_queue: " << ratio(result) << '\n';
}

void print_queue_json(std::ostream& out, const Environment& environment,
                      const QueueBenchmarkConfig& config, const QueueBenchmarkResult& result) {
    const auto print_list = [&out](const std::vector<double>& values) {
        out << '[';
        for (std::size_t i = 0; i < values.size(); ++i) {
            out << (i == 0 ? "" : ", ") << values[i];
        }
        out << ']';
    };

    out << "{\n  \"benchmark\": \"queue\",\n"
        << "  \"environment\": {\"compiler\": \"" << environment.compiler
        << "\", \"build_type\": \"" << environment.build_type << "\", \"compiler_flags\": \""
        << environment.compiler_flags << "\", \"operating_system\": \""
        << environment.operating_system << "\", \"wsl\": " << (environment.wsl ? "true" : "false")
        << ", \"cpu_model\": \"" << environment.cpu_model
        << "\", \"logical_cpus\": " << environment.logical_cpus << "},\n"
        << "  \"method\": {\"messages\": " << config.messages
        << ", \"message_size\": " << result.message_size << ", \"capacity\": " << config.capacity
        << ", \"runs\": " << config.runs << "},\n"
        << std::fixed << std::setprecision(0) << "  \"spsc_messages_per_second\": ";
    print_list(result.spsc_throughput);
    out << ",\n  \"mutex_messages_per_second\": ";
    print_list(result.mutex_throughput);
    out << ",\n  \"median\": {\"spsc_messages_per_second\": " << median(result.spsc_throughput)
        << ", \"mutex_messages_per_second\": " << median(result.mutex_throughput)
        << ", \"ratio\": " << std::setprecision(2) << ratio(result) << "}\n}\n";
}

} // namespace exchange::bench
