#include "engine_benchmark.hpp"
#include "environment.hpp"
#include "queue_benchmark.hpp"
#include "report.hpp"
#include "workload.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using exchange::bench::EngineBenchmarkConfig;

constexpr std::string_view usage =
    "Usage: exchange-bench engine [OPTIONS]\n"
    "       exchange-bench queue [OPTIONS]\n"
    "\n"
    "engine: measures the matching engine alone: no networking, no queues, one thread.\n"
    "\n"
    "  --commands N            measured commands (default 1000000)\n"
    "  --initial-orders N      resting orders placed before measuring (default 10000)\n"
    "  --sessions N            sessions the orders are spread over (default 8)\n"
    "  --price-levels N        price levels used on each side (default 50)\n"
    "  --cancel-percent P      share of commands that are cancels (default 40)\n"
    "  --aggressive-percent P  share of commands that trade immediately (default 10)\n"
    "  --seed N                seed of the workload generator (default 42)\n"
    "  --runs N                measured repetitions (default 5)\n"
    "  --warmup-runs N         unmeasured repetitions run first (default 1)\n"
    "  --clock auto|tsc|steady clock used for latencies (default auto: the processor's\n"
    "                          timestamp counter if usable, otherwise steady_clock)\n"
    "  --json FILE             also write the results to FILE as JSON\n"
    "\n"
    "queue: compares the lock-free SPSC queue with a mutex-protected queue, sending\n"
    "commands from one thread to another.\n"
    "\n"
    "  --messages N            messages transferred per run (default 10000000)\n"
    "  --capacity N            queue capacity (default 4096)\n"
    "  --runs N                repetitions (default 5)\n"
    "  --json FILE             also write the results to FILE as JSON\n";

template <typename T> bool parse_number(std::string_view text, T& value) {
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && parsed_end == end;
}

int fail(std::string_view message) {
    std::cerr << "error: " << message << "\n\n" << usage;
    return EXIT_FAILURE;
}

// Each option name is mapped to the code that stores its value.
using Setter = std::function<bool(std::string_view)>;
using Options = std::map<std::string_view, Setter>;

template <typename T> Setter number_into(T& target) {
    return [&target](std::string_view text) { return parse_number(text, target); };
}

Setter text_into(std::string& target) {
    return [&target](std::string_view text) {
        target = text;
        return !target.empty();
    };
}

// Reads "--name value" pairs. Returns an empty string, or a description of the
// first problem.
std::string parse_options(std::span<char*> arguments, const Options& options) {
    for (std::size_t i = 0; i < arguments.size(); i += 2) {
        const std::string_view name = arguments[i];
        const auto option = options.find(name);
        if (option == options.end()) {
            return "unknown option " + std::string(name);
        }
        if (i + 1 >= arguments.size() || !option->second(arguments[i + 1])) {
            return "missing or invalid value for " + std::string(name);
        }
    }
    return {};
}

exchange::bench::Environment environment_with_warning() {
    exchange::bench::Environment environment = exchange::bench::detect_environment();
    const std::string warning = exchange::bench::performance_warning(environment);
    if (!warning.empty()) {
        std::cerr << "warning: " << warning
                  << "; these numbers say nothing about performance. Use the release preset.\n\n";
    }
    return environment;
}

int run_engine(std::span<char*> arguments) {
    EngineBenchmarkConfig config;
    std::string json_path;

    const Options options = {
        {"--commands", number_into(config.workload.commands)},
        {"--initial-orders", number_into(config.workload.initial_orders)},
        {"--sessions", number_into(config.workload.sessions)},
        {"--price-levels", number_into(config.workload.price_levels)},
        {"--cancel-percent", number_into(config.workload.cancel_percent)},
        {"--aggressive-percent", number_into(config.workload.aggressive_percent)},
        {"--seed", number_into(config.workload.seed)},
        {"--runs", number_into(config.runs)},
        {"--warmup-runs", number_into(config.warmup_runs)},
        {"--clock",
         [&config](std::string_view text) {
             using exchange::bench::ClockChoice;
             const std::map<std::string_view, ClockChoice> choices = {
                 {"auto", ClockChoice::Automatic},
                 {"tsc", ClockChoice::Tsc},
                 {"steady", ClockChoice::Steady},
             };
             const auto choice = choices.find(text);
             if (choice == choices.end()) {
                 return false;
             }
             config.clock = choice->second;
             return true;
         }},
        {"--json", text_into(json_path)},
    };
    const std::string option_problem = parse_options(arguments, options);
    if (!option_problem.empty()) {
        return fail(option_problem);
    }

    const std::string problem = exchange::bench::validate(config.workload);
    if (!problem.empty()) {
        return fail(problem);
    }
    if (config.workload.commands == 0 || config.runs == 0) {
        return fail("commands and runs must be at least 1");
    }

    const exchange::bench::Environment environment = environment_with_warning();

    const exchange::bench::EngineBenchmarkResult result =
        exchange::bench::run_engine_benchmark(config);
    if (!result.error.empty()) {
        std::cerr << "error: " << result.error << '\n';
        return EXIT_FAILURE;
    }

    exchange::bench::print_engine_report(std::cout, environment, config, result);
    if (!json_path.empty()) {
        std::ofstream json(json_path);
        if (!json) {
            std::cerr << "error: cannot write " << json_path << '\n';
            return EXIT_FAILURE;
        }
        exchange::bench::print_engine_json(json, environment, config, result);
    }
    return EXIT_SUCCESS;
}

int run_queue(std::span<char*> arguments) {
    exchange::bench::QueueBenchmarkConfig config;
    std::string json_path;

    const Options options = {
        {"--messages", number_into(config.messages)},
        {"--capacity", number_into(config.capacity)},
        {"--runs", number_into(config.runs)},
        {"--json", text_into(json_path)},
    };
    const std::string option_problem = parse_options(arguments, options);
    if (!option_problem.empty()) {
        return fail(option_problem);
    }
    if (config.messages == 0 || config.capacity == 0 || config.runs == 0) {
        return fail("messages, capacity and runs must be at least 1");
    }

    const exchange::bench::Environment environment = environment_with_warning();

    const exchange::bench::QueueBenchmarkResult result =
        exchange::bench::run_queue_benchmark(config);
    if (!result.error.empty()) {
        std::cerr << "error: " << result.error << '\n';
        return EXIT_FAILURE;
    }

    exchange::bench::print_queue_report(std::cout, environment, config, result);
    if (!json_path.empty()) {
        std::ofstream json(json_path);
        if (!json) {
            std::cerr << "error: cannot write " << json_path << '\n';
            return EXIT_FAILURE;
        }
        exchange::bench::print_queue_json(json, environment, config, result);
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() < 2) {
        std::cerr << usage;
        return EXIT_FAILURE;
    }
    const std::string_view command = arguments[1];
    if (command == "-h" || command == "--help") {
        std::cout << usage;
        return EXIT_SUCCESS;
    }
    if (command == "engine") {
        return run_engine(arguments.subspan(2));
    }
    if (command == "queue") {
        return run_queue(arguments.subspan(2));
    }
    return fail("unknown benchmark " + std::string(command));
}
