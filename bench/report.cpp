#include "report.hpp"

#include <cstdint>
#include <functional>
#include <iomanip>
#include <ostream>
#include <string>
#include <string_view>

namespace exchange::bench {

namespace {

// The median, over all runs, of one statistic of a run.
double median_of(const std::vector<EngineRunResult>& runs,
                 const std::function<double(const EngineRunResult&)>& statistic) {
    std::vector<double> values;
    values.reserve(runs.size());
    for (const EngineRunResult& run : runs) {
        values.push_back(statistic(run));
    }
    return median(std::move(values));
}

double percent_of(std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

void print_run_row(std::ostream& out, std::string_view label, const EngineRunResult& run) {
    const LatencySummary& latency = run.latency_ns;
    out << "  " << std::left << std::setw(7) << label << std::right << std::setw(8) << latency.min
        << std::setw(8) << latency.p50 << std::setw(8) << latency.p95 << std::setw(8) << latency.p99
        << std::setw(9) << latency.p999 << std::setw(11) << latency.max << std::setw(9)
        << std::fixed << std::setprecision(1) << latency.mean << std::setw(14)
        << std::setprecision(0) << run.commands_per_second << '\n';
}

// Writes a string as a JSON string literal.
void print_json_string(std::ostream& out, std::string_view text) {
    out << '"';
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            out << '\\' << character;
        } else if (static_cast<unsigned char>(character) < 0x20) {
            out << ' ';
        } else {
            out << character;
        }
    }
    out << '"';
}

void print_json_run(std::ostream& out, const EngineRunResult& run) {
    const LatencySummary& latency = run.latency_ns;
    out << "{\"latency_ns\": {\"min\": " << latency.min << ", \"p50\": " << latency.p50
        << ", \"p95\": " << latency.p95 << ", \"p99\": " << latency.p99
        << ", \"p99_9\": " << latency.p999 << ", \"max\": " << latency.max
        << ", \"mean\": " << std::fixed << std::setprecision(1) << latency.mean
        << "}, \"commands_per_second\": " << std::setprecision(0) << run.commands_per_second << "}";
}

} // namespace

EngineRunResult median_run(const std::vector<EngineRunResult>& runs) {
    const auto latency = [&](std::uint64_t LatencySummary::* field) {
        return static_cast<std::uint64_t>(median_of(runs, [field](const EngineRunResult& run) {
            return static_cast<double>(run.latency_ns.*field);
        }));
    };
    EngineRunResult summary;
    summary.latency_ns.count = runs.empty() ? 0 : runs.front().latency_ns.count;
    summary.latency_ns.min = latency(&LatencySummary::min);
    summary.latency_ns.p50 = latency(&LatencySummary::p50);
    summary.latency_ns.p95 = latency(&LatencySummary::p95);
    summary.latency_ns.p99 = latency(&LatencySummary::p99);
    summary.latency_ns.p999 = latency(&LatencySummary::p999);
    summary.latency_ns.max = latency(&LatencySummary::max);
    summary.latency_ns.mean =
        median_of(runs, [](const EngineRunResult& run) { return run.latency_ns.mean; });
    summary.commands_per_second =
        median_of(runs, [](const EngineRunResult& run) { return run.commands_per_second; });
    return summary;
}

void print_engine_report(std::ostream& out, const Environment& environment,
                         const EngineBenchmarkConfig& config, const EngineBenchmarkResult& result) {
    const WorkloadConfig& workload = config.workload;
    const WorkloadStats& stats = result.workload;
    const std::uint64_t commands = stats.passive_orders + stats.aggressive_orders + stats.cancels;

    out << "Engine benchmark: MatchingEngine::process() on one thread, no networking, no "
           "queues\n\n";

    out << "Environment\n"
        << "  compiler     " << environment.compiler << '\n'
        << "  build        " << environment.build_type << " (" << environment.compiler_flags
        << ")\n"
        << "  system       " << environment.operating_system
        << (environment.wsl ? " [WSL2: virtualized, timings are indicative]" : "") << '\n'
        << "  cpu          " << environment.cpu_model << ", " << environment.logical_cpus
        << " logical CPUs\n\n";

    out << "Workload\n"
        << "  seed " << workload.seed << ", " << workload.initial_orders << " initial orders, "
        << workload.commands << " measured commands, " << workload.sessions << " sessions\n"
        << "  prices within " << workload.price_levels << " ticks of " << workload.mid_price
        << ", quantities 1 to " << workload.max_quantity << '\n'
        << std::fixed << std::setprecision(1)
        << "  measured mix: " << percent_of(stats.passive_orders, commands) << "% passive orders, "
        << percent_of(stats.aggressive_orders, commands) << "% aggressive orders, "
        << percent_of(stats.cancels, commands) << "% cancels\n"
        << "  produced " << stats.trades << " trades and " << stats.events << " events\n"
        << "  book at the end: " << stats.final_orders << " orders on " << stats.final_bid_levels
        << " bid levels and " << stats.final_ask_levels << " ask levels\n\n";

    out << "Method\n"
        << "  " << config.warmup_runs << " warm-up run(s), then " << config.runs
        << " measured run(s) of the same commands on a new engine\n"
        << "  latency: the " << result.clock.name
        << " clock is read before and after each process() call\n"
        << "  clock: smallest measurable interval " << std::setprecision(1)
        << result.clock.smallest_interval_ns << " ns, about " << result.clock.reading_cost_ns
        << " ns per reading, included in every latency\n"
        << "  throughput: a separate pass timed as a whole\n\n";

    out << "Latency per command in nanoseconds, throughput in commands per second\n"
        << "  run         min     p50     p95     p99    p99.9        max     mean    throughput\n";
    for (std::size_t i = 0; i < result.runs.size(); ++i) {
        print_run_row(out, std::to_string(i + 1), result.runs[i]);
    }
    print_run_row(out, "median", median_run(result.runs));
}

void print_engine_json(std::ostream& out, const Environment& environment,
                       const EngineBenchmarkConfig& config, const EngineBenchmarkResult& result) {
    const WorkloadConfig& workload = config.workload;
    const WorkloadStats& stats = result.workload;

    out << "{\n  \"benchmark\": \"engine\",\n  \"environment\": {\"compiler\": ";
    print_json_string(out, environment.compiler);
    out << ", \"build_type\": ";
    print_json_string(out, environment.build_type);
    out << ", \"compiler_flags\": ";
    print_json_string(out, environment.compiler_flags);
    out << ", \"sanitizer\": ";
    print_json_string(out, environment.sanitizer);
    out << ", \"operating_system\": ";
    print_json_string(out, environment.operating_system);
    out << ", \"wsl\": " << (environment.wsl ? "true" : "false") << ", \"cpu_model\": ";
    print_json_string(out, environment.cpu_model);
    out << ", \"logical_cpus\": " << environment.logical_cpus << "},\n";

    out << "  \"workload\": {\"seed\": " << workload.seed
        << ", \"initial_orders\": " << workload.initial_orders
        << ", \"commands\": " << workload.commands << ", \"sessions\": " << workload.sessions
        << ", \"price_levels\": " << workload.price_levels
        << ", \"cancel_percent\": " << workload.cancel_percent
        << ", \"aggressive_percent\": " << workload.aggressive_percent
        << ", \"mid_price\": " << workload.mid_price
        << ", \"max_quantity\": " << workload.max_quantity << "},\n";

    out << "  \"workload_stats\": {\"passive_orders\": " << stats.passive_orders
        << ", \"aggressive_orders\": " << stats.aggressive_orders
        << ", \"cancels\": " << stats.cancels << ", \"trades\": " << stats.trades
        << ", \"events\": " << stats.events << ", \"final_orders\": " << stats.final_orders
        << ", \"final_bid_levels\": " << stats.final_bid_levels
        << ", \"final_ask_levels\": " << stats.final_ask_levels << "},\n";

    out << "  \"method\": {\"warmup_runs\": " << config.warmup_runs << ", \"runs\": " << config.runs
        << ", \"clock\": ";
    print_json_string(out, result.clock.name);
    out << std::fixed << std::setprecision(3)
        << ", \"clock_ticks_per_nanosecond\": " << result.clock.ticks_per_nanosecond
        << std::setprecision(1)
        << ", \"clock_smallest_interval_ns\": " << result.clock.smallest_interval_ns
        << ", \"clock_reading_cost_ns\": " << result.clock.reading_cost_ns << "},\n";

    out << "  \"runs\": [";
    for (std::size_t i = 0; i < result.runs.size(); ++i) {
        out << (i == 0 ? "\n    " : ",\n    ");
        print_json_run(out, result.runs[i]);
    }
    out << "\n  ],\n  \"median\": ";
    print_json_run(out, median_run(result.runs));
    out << "\n}\n";
}

} // namespace exchange::bench
