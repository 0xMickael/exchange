#include "engine_benchmark.hpp"

#include "exchange/core/matching_engine.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>

namespace exchange::bench {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t nanoseconds_between(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

// Large enough that appending the events of one command never reallocates.
constexpr std::size_t event_buffer_capacity = 4096;

// A new engine with the setup commands applied: the state every pass starts from.
MatchingEngine prepared_engine(const Workload& workload, std::vector<Event>& events) {
    MatchingEngine engine;
    for (const Command& command : workload.setup) {
        events.clear();
        engine.process(command, events);
    }
    return engine;
}

// Times every command individually and stores the latencies, in nanoseconds,
// in `samples`. Returns the number of events produced, so that the work cannot
// be optimized away, or nullopt if the clock did not agree with steady_clock
// over the whole pass.
std::optional<std::size_t> latency_pass(const Workload& workload, const BenchClock& clock,
                                        std::vector<std::uint64_t>& samples) {
    std::vector<Event> events;
    events.reserve(event_buffer_capacity);
    MatchingEngine engine = prepared_engine(workload, events);

    std::size_t event_count = 0;
    const Clock::time_point wall_start = Clock::now();
    const std::uint64_t first = clock.ticks();
    for (std::size_t i = 0; i < workload.measured.size(); ++i) {
        events.clear();
        const std::uint64_t start = clock.ticks();
        engine.process(workload.measured[i], events);
        const std::uint64_t end = clock.ticks();
        samples[i] = end - start;
        event_count += events.size();
    }
    const std::uint64_t last = clock.ticks();
    const Clock::time_point wall_end = Clock::now();

    for (std::uint64_t& sample : samples) {
        sample = clock.to_nanoseconds(sample);
    }

    // A TSC that changed rate or jumped between processor cores would make the
    // two clocks disagree about the duration of the pass.
    const auto wall_ns = static_cast<double>(nanoseconds_between(wall_start, wall_end));
    const auto clock_ns = static_cast<double>(clock.to_nanoseconds(last - first));
    constexpr double tolerance = 0.01;
    constexpr double slack_ns = 100'000.0;
    if (std::abs(wall_ns - clock_ns) > (tolerance * wall_ns) + slack_ns) {
        return std::nullopt;
    }
    return event_count;
}

// Times the whole stream with two clock readings, so the clock's own cost does
// not reduce the measured throughput.
double throughput_pass(const Workload& workload, std::size_t& event_count) {
    std::vector<Event> events;
    events.reserve(event_buffer_capacity);
    MatchingEngine engine = prepared_engine(workload, events);

    event_count = 0;
    const Clock::time_point start = Clock::now();
    for (const Command& command : workload.measured) {
        events.clear();
        engine.process(command, events);
        event_count += events.size();
    }
    const Clock::time_point end = Clock::now();

    const double seconds = static_cast<double>(nanoseconds_between(start, end)) / 1e9;
    return seconds > 0.0 ? static_cast<double>(workload.measured.size()) / seconds : 0.0;
}

} // namespace

EngineBenchmarkResult run_engine_benchmark(const EngineBenchmarkConfig& config) {
    EngineBenchmarkResult result;

    const std::optional<BenchClock> clock = BenchClock::create(config.clock);
    if (!clock.has_value()) {
        result.error = "the TSC clock is not usable on this machine";
        return result;
    }
    result.clock = ClockDescription{.name = clock->name(),
                                    .ticks_per_nanosecond = 1.0 / clock->ns_per_tick(),
                                    .smallest_interval_ns = clock->smallest_interval_ns(),
                                    .reading_cost_ns = clock->reading_cost_ns()};

    const Workload workload = generate_workload(config.workload);
    result.workload = workload.stats;

    // Allocated and written once before any timing, so that no page fault for
    // this buffer lands inside a measurement.
    std::vector<std::uint64_t> samples(workload.measured.size(), 0);

    const auto timed_pass = [&]() {
        const std::optional<std::size_t> events = latency_pass(workload, *clock, samples);
        if (!events.has_value()) {
            result.error = "the " + clock->name() + " clock disagreed with steady_clock";
        } else if (*events != workload.stats.events) {
            result.error = "a measured pass did not produce the expected events";
        }
        return result.error.empty();
    };

    for (std::uint32_t i = 0; i < config.warmup_runs; ++i) {
        if (!timed_pass()) {
            return result;
        }
    }

    for (std::uint32_t i = 0; i < config.runs; ++i) {
        if (!timed_pass()) {
            return result;
        }
        EngineRunResult run;
        run.latency_ns = summarize(samples);

        // Every pass replays the same deterministic stream, so the number of
        // events is known in advance.
        std::size_t throughput_events = 0;
        run.commands_per_second = throughput_pass(workload, throughput_events);
        if (throughput_events != workload.stats.events) {
            result.error = "a measured pass did not produce the expected events";
            return result;
        }
        result.runs.push_back(run);
    }
    return result;
}

} // namespace exchange::bench
