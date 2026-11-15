#include "e2e_benchmark.hpp"
#include "engine_benchmark.hpp"
#include "environment.hpp"
#include "latency.hpp"
#include "mutex_queue.hpp"
#include "queue_benchmark.hpp"
#include "report.hpp"
#include "workload.hpp"

#include "exchange/core/matching_engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace exchange::bench {
namespace {

// --- Percentiles ---

TEST(Latency, PercentileUsesTheNearestRank) {
    // 1, 2, ..., 100: the p-th percentile is p.
    std::vector<std::uint64_t> samples;
    for (std::uint64_t value = 1; value <= 100; ++value) {
        samples.push_back(value);
    }

    EXPECT_EQ(percentile(samples, 5'000), 50U);
    EXPECT_EQ(percentile(samples, 9'500), 95U);
    EXPECT_EQ(percentile(samples, 9'900), 99U);
    EXPECT_EQ(percentile(samples, 9'990), 100U); // ceil(99.9) = 100
    EXPECT_EQ(percentile(samples, 10'000), 100U);
    EXPECT_EQ(percentile(samples, 0), 1U);
    EXPECT_EQ(percentile(samples, 1), 1U);
}

TEST(Latency, PercentileOfASingleSample) {
    const std::vector<std::uint64_t> samples = {7};

    EXPECT_EQ(percentile(samples, 5'000), 7U);
    EXPECT_EQ(percentile(samples, 9'990), 7U);
}

TEST(Latency, SummarizeSortsAndDescribesTheSamples) {
    std::vector<std::uint64_t> samples = {50, 10, 40, 20, 30};

    const LatencySummary summary = summarize(samples);

    EXPECT_EQ(summary.count, 5U);
    EXPECT_EQ(summary.min, 10U);
    EXPECT_EQ(summary.p50, 30U);
    EXPECT_EQ(summary.p99, 50U);
    EXPECT_EQ(summary.max, 50U);
    EXPECT_DOUBLE_EQ(summary.mean, 30.0);
}

TEST(Latency, SummaryOfNothingIsAllZeros) {
    std::vector<std::uint64_t> samples;

    EXPECT_EQ(summarize(samples).count, 0U);
}

TEST(Latency, MedianTakesTheLowerMiddleValue) {
    EXPECT_DOUBLE_EQ(median({3.0, 1.0, 2.0}), 2.0);
    EXPECT_DOUBLE_EQ(median({4.0, 1.0, 3.0, 2.0}), 2.0);
    EXPECT_DOUBLE_EQ(median({}), 0.0);
}

// --- Workload generation ---

WorkloadConfig small_workload() {
    WorkloadConfig config;
    config.initial_orders = 200;
    config.commands = 5'000;
    return config;
}

TEST(Workload, SameConfigurationGivesTheSameCommands) {
    const Workload first = generate_workload(small_workload());
    const Workload second = generate_workload(small_workload());

    EXPECT_EQ(first.setup, second.setup);
    EXPECT_EQ(first.measured, second.measured);
}

TEST(Workload, DifferentSeedsGiveDifferentCommands) {
    WorkloadConfig other = small_workload();
    other.seed = 43;

    EXPECT_NE(generate_workload(small_workload()).measured, generate_workload(other).measured);
}

TEST(Workload, HasTheRequestedSize) {
    const Workload workload = generate_workload(small_workload());

    EXPECT_EQ(workload.setup.size(), 200U);
    EXPECT_EQ(workload.measured.size(), 5'000U);
    EXPECT_EQ(workload.stats.passive_orders + workload.stats.aggressive_orders +
                  workload.stats.cancels,
              5'000U);
}

TEST(Workload, EveryCommandIsAcceptedByTheEngine) {
    const Workload workload = generate_workload(small_workload());
    MatchingEngine engine;
    std::vector<Event> events;

    for (const std::vector<Command>* commands : {&workload.setup, &workload.measured}) {
        for (const Command& command : *commands) {
            engine.process(command, events);
        }
    }

    std::uint64_t trades = 0;
    for (const Event& event : events) {
        EXPECT_FALSE(std::holds_alternative<OrderRejected>(event));
        EXPECT_FALSE(std::holds_alternative<CancelRejected>(event));
        trades += std::holds_alternative<Trade>(event) ? 1U : 0U;
    }
    EXPECT_GT(trades, 0U);
    EXPECT_EQ(engine.check_invariants(), "");
    EXPECT_EQ(engine.book().order_count(), workload.stats.final_orders);
}

TEST(Workload, MixFollowsTheConfiguredPercentages) {
    WorkloadConfig config = small_workload();
    config.initial_orders = 2'000;
    config.commands = 20'000;
    config.cancel_percent = 30;
    config.aggressive_percent = 20;

    const WorkloadStats stats = generate_workload(config).stats;

    // Within two percentage points of the requested shares.
    EXPECT_NEAR(static_cast<double>(stats.cancels), 6'000.0, 400.0);
    EXPECT_NEAR(static_cast<double>(stats.aggressive_orders), 4'000.0, 400.0);
    EXPECT_NEAR(static_cast<double>(stats.passive_orders), 10'000.0, 400.0);
}

TEST(Workload, ValidationRejectsUnusableConfigurations) {
    EXPECT_EQ(validate(WorkloadConfig{}), "");

    WorkloadConfig no_sessions;
    no_sessions.sessions = 0;
    EXPECT_NE(validate(no_sessions), "");

    WorkloadConfig too_many_percent;
    too_many_percent.cancel_percent = 70;
    too_many_percent.aggressive_percent = 40;
    EXPECT_NE(validate(too_many_percent), "");

    WorkloadConfig prices_below_minimum;
    prices_below_minimum.mid_price = 10;
    prices_below_minimum.price_levels = 50;
    EXPECT_NE(validate(prices_below_minimum), "");
}

// --- Benchmark run ---

TEST(EngineBenchmark, RunsAndReportsEveryRun) {
    EngineBenchmarkConfig config;
    config.workload = small_workload();
    config.runs = 2;
    config.warmup_runs = 1;
    // The portable clock, so that the test does not depend on the machine's TSC.
    config.clock = ClockChoice::Steady;

    const EngineBenchmarkResult result = run_engine_benchmark(config);

    EXPECT_EQ(result.error, "");
    EXPECT_EQ(result.clock.name, "steady_clock");
    EXPECT_GT(result.clock.smallest_interval_ns, 0.0);
    ASSERT_EQ(result.runs.size(), 2U);
    for (const EngineRunResult& run : result.runs) {
        EXPECT_EQ(run.latency_ns.count, 5'000U);
        EXPECT_LE(run.latency_ns.min, run.latency_ns.p50);
        EXPECT_LE(run.latency_ns.p50, run.latency_ns.p99);
        EXPECT_LE(run.latency_ns.p99, run.latency_ns.max);
        EXPECT_GT(run.commands_per_second, 0.0);
    }
    EXPECT_GT(result.workload.events, 0U);
}

TEST(EngineBenchmark, ReportsContainTheResults) {
    EngineBenchmarkConfig config;
    config.workload = small_workload();
    config.runs = 1;
    config.warmup_runs = 0;
    config.clock = ClockChoice::Steady;
    const EngineBenchmarkResult result = run_engine_benchmark(config);
    const Environment environment = detect_environment();

    std::ostringstream text;
    print_engine_report(text, environment, config, result);
    std::ostringstream json;
    print_engine_json(json, environment, config, result);

    EXPECT_NE(text.str().find("median"), std::string::npos);
    EXPECT_NE(text.str().find(environment.compiler), std::string::npos);
    EXPECT_NE(json.str().find("\"benchmark\": \"engine\""), std::string::npos);
    EXPECT_NE(json.str().find("\"commands\": 5000"), std::string::npos);
    EXPECT_EQ(json.str().front(), '{');
}

TEST(BenchClock, MeasuresAKnownInterval) {
    const std::optional<BenchClock> clock = BenchClock::create(ClockChoice::Automatic);
    if (!clock.has_value()) {
        FAIL() << "no clock is available";
    }
    const BenchClock& bench_clock = *clock;

    const auto wall_start = std::chrono::steady_clock::now();
    const std::uint64_t first = bench_clock.ticks();
    while (std::chrono::steady_clock::now() - wall_start < std::chrono::milliseconds(20)) {
    }
    const std::uint64_t last = bench_clock.ticks();
    const double wall_ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - wall_start)
            .count();

    EXPECT_NEAR(static_cast<double>(bench_clock.to_nanoseconds(last - first)), wall_ns,
                wall_ns * 0.05);
    EXPECT_GT(bench_clock.reading_cost_ns(), 0.0);
}

// --- Queue benchmark ---

TEST(QueueBenchmark, TransfersEveryMessageThroughBothQueues) {
    QueueBenchmarkConfig config;
    config.messages = 20'000;
    config.capacity = 64;
    config.runs = 2;

    const QueueBenchmarkResult result = run_queue_benchmark(config);

    EXPECT_EQ(result.error, "");
    EXPECT_GT(result.message_size, 0U);
    ASSERT_EQ(result.spsc_throughput.size(), 2U);
    ASSERT_EQ(result.mutex_throughput.size(), 2U);
    EXPECT_GT(result.spsc_throughput[0], 0.0);
    EXPECT_GT(result.mutex_throughput[0], 0.0);

    std::ostringstream json;
    print_queue_json(json, detect_environment(), config, result);
    EXPECT_NE(json.str().find("\"benchmark\": \"queue\""), std::string::npos);
}

TEST(MutexQueue, BehavesLikeABoundedFifo) {
    MutexQueue<int> queue(2);
    int value = 0;

    EXPECT_FALSE(queue.try_pop(value));
    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_FALSE(queue.try_push(3));
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.try_push(3));
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 3);
    EXPECT_FALSE(queue.try_pop(value));
}

// --- End-to-end benchmark ---

E2eConfig small_e2e() {
    E2eConfig config;
    config.workload.initial_orders = 100;
    config.connections = 2;
    config.requests = 300;
    config.runs = 1;
    // The portable clock, so that the test does not depend on the machine's TSC.
    config.clock = ClockChoice::Steady;
    return config;
}

TEST(E2eBenchmark, ClosedLoopMeasuresEveryRequestOnEveryServer) {
    const E2eConfig config = small_e2e();

    const E2eResult result = run_e2e_benchmark(config);

    EXPECT_EQ(result.error, "");
    ASSERT_EQ(result.servers.size(), 3U);
    for (const E2eServerResult& server : result.servers) {
        ASSERT_EQ(server.runs.size(), 1U) << to_string(server.kind);
        EXPECT_EQ(server.runs[0].latency_ns.count, 600U) << to_string(server.kind);
        EXPECT_GT(server.runs[0].latency_ns.p50, 0U);
        EXPECT_GT(server.runs[0].commands_per_second, 0.0);
    }
}

TEST(E2eBenchmark, OpenLoopKeepsToItsSchedule) {
    E2eConfig config = small_e2e();
    config.servers = {ServerKind::PipelineBackoff};
    config.connections = 1;
    config.requests = 400;
    config.rate = 2000.0; // 400 requests at 2000 per second take 0.2 s.

    const E2eResult result = run_e2e_benchmark(config);

    EXPECT_EQ(result.error, "");
    ASSERT_EQ(result.servers.size(), 1U);
    EXPECT_EQ(result.servers[0].runs[0].latency_ns.count, 400U);
    // The measured rate cannot exceed the scheduled one.
    EXPECT_LE(result.servers[0].runs[0].commands_per_second, 2000.0 * 1.05);
}

TEST(E2eBenchmark, ReportsContainEveryServer) {
    E2eConfig config = small_e2e();
    config.connections = 1;
    const E2eResult result = run_e2e_benchmark(config);
    const Environment environment = detect_environment();

    std::ostringstream text;
    print_e2e_report(text, environment, config, result);
    std::ostringstream json;
    print_e2e_json(json, environment, config, result);

    for (const std::string name : {"inline", "pipeline (backoff)", "pipeline (spin)"}) {
        EXPECT_NE(text.str().find(name), std::string::npos) << name;
        EXPECT_NE(json.str().find("\"server\": \"" + name + "\""), std::string::npos) << name;
    }
    EXPECT_NE(text.str().find("closed loop"), std::string::npos);
    EXPECT_NE(json.str().find("\"loop\": \"closed\""), std::string::npos);
}

TEST(Environment, IsDetected) {
    const Environment environment = detect_environment();

    EXPECT_FALSE(environment.compiler.empty());
    EXPECT_FALSE(environment.operating_system.empty());
    EXPECT_GT(environment.logical_cpus, 0U);
}

} // namespace
} // namespace exchange::bench
