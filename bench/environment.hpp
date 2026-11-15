#pragma once

#include <string>

namespace exchange::bench {

// Where and how the benchmark binary was built and is running. Reported with
// every result, because a number without this context cannot be compared.
struct Environment {
    std::string compiler;
    std::string build_type;
    std::string compiler_flags;
    std::string sanitizer; // Empty if none.
    bool assertions_enabled = false;
    std::string operating_system;
    bool wsl = false;
    std::string cpu_model;
    unsigned logical_cpus = 0;
};

[[nodiscard]] Environment detect_environment();

// Empty if results from this build are meaningful, otherwise the reason they are not.
[[nodiscard]] std::string performance_warning(const Environment& environment);

} // namespace exchange::bench
