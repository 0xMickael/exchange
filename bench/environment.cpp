#include "environment.hpp"

#include <fstream>
#include <string_view>
#include <thread>

#include <sys/utsname.h>

namespace exchange::bench {

namespace {

std::string compiler_name() {
#ifdef __clang__
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "unknown";
#endif
}

std::string cpu_model_name() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.starts_with("model name")) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::size_t start = line.find_first_not_of(" \t", colon + 1);
                return start == std::string::npos ? std::string{} : line.substr(start);
            }
        }
    }
    return "unknown";
}

} // namespace

Environment detect_environment() {
    Environment environment;
    environment.compiler = compiler_name();
    environment.build_type = EXCHANGE_BUILD_TYPE;
    environment.compiler_flags = EXCHANGE_CXX_FLAGS;
    environment.sanitizer = EXCHANGE_SANITIZER_NAME;
#ifdef NDEBUG
    environment.assertions_enabled = false;
#else
    environment.assertions_enabled = true;
#endif

    utsname system_info{};
    if (uname(&system_info) == 0) {
        const std::string_view release = static_cast<const char*>(system_info.release);
        environment.operating_system =
            std::string(static_cast<const char*>(system_info.sysname)) + " " + std::string(release);
        environment.wsl = release.find("microsoft") != std::string_view::npos ||
                          release.find("WSL") != std::string_view::npos;
    } else {
        environment.operating_system = "unknown";
    }

    environment.cpu_model = cpu_model_name();
    environment.logical_cpus = std::thread::hardware_concurrency();
    return environment;
}

std::string performance_warning(const Environment& environment) {
    if (!environment.sanitizer.empty()) {
        return "built with a sanitizer (" + environment.sanitizer + ")";
    }
    if (environment.assertions_enabled) {
        return "not an optimized build (build type: " + environment.build_type + ")";
    }
    return {};
}

} // namespace exchange::bench
