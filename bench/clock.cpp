#include "clock.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ratio>

namespace exchange::bench {

namespace {

using Steady = std::chrono::steady_clock;

double steady_ns_per_tick() {
    return static_cast<double>(std::nano::den) * static_cast<double>(Steady::period::num) /
           static_cast<double>(Steady::period::den);
}

#ifdef __x86_64__
// Counts TSC ticks during a fixed steady_clock interval.
double calibrate_tsc_ns_per_tick() {
    constexpr std::chrono::milliseconds interval(50);
    const Steady::time_point start = Steady::now();
    const std::uint64_t first = __rdtsc();
    Steady::time_point now = start;
    while (now - start < interval) {
        now = Steady::now();
    }
    const std::uint64_t last = __rdtsc();
    const double elapsed_ns = std::chrono::duration<double, std::nano>(now - start).count();
    return last > first ? elapsed_ns / static_cast<double>(last - first) : 0.0;
}
#endif

} // namespace

BenchClock::BenchClock(ClockKind kind, double ns_per_tick)
    : kind_(kind), ns_per_tick_(ns_per_tick) {
    constexpr std::size_t readings = 200'000;
    std::uint64_t smallest = 0;
    const std::uint64_t first = ticks();
    std::uint64_t previous = first;
    for (std::size_t i = 0; i < readings; ++i) {
        const std::uint64_t current = ticks();
        const std::uint64_t step = current - previous;
        if (step != 0 && (smallest == 0 || step < smallest)) {
            smallest = step;
        }
        previous = current;
    }
    smallest_interval_ns_ = static_cast<double>(smallest) * ns_per_tick_;
    reading_cost_ns_ =
        static_cast<double>(previous - first) * ns_per_tick_ / static_cast<double>(readings);
}

BenchClock BenchClock::steady() {
    return {ClockKind::Steady, steady_ns_per_tick()};
}

std::optional<BenchClock> BenchClock::tsc() {
#ifdef __x86_64__
    // The processor flag that guarantees a constant TSC rate is often hidden
    // inside virtual machines, so stability is checked by measuring twice.
    const double first = calibrate_tsc_ns_per_tick();
    const double second = calibrate_tsc_ns_per_tick();
    if (first <= 0.0 || second <= 0.0) {
        return std::nullopt;
    }
    constexpr double tolerance = 0.001;
    if (std::abs(first - second) > tolerance * std::max(first, second)) {
        return std::nullopt;
    }
    return BenchClock(ClockKind::Tsc, (first + second) / 2.0);
#else
    return std::nullopt;
#endif
}

std::optional<BenchClock> BenchClock::create(ClockChoice choice) {
    if (choice == ClockChoice::Steady) {
        return steady();
    }
    std::optional<BenchClock> clock = tsc();
    if (!clock.has_value() && choice == ClockChoice::Automatic) {
        return steady();
    }
    return clock;
}

std::string BenchClock::name() const {
    return kind_ == ClockKind::Tsc ? "tsc" : "steady_clock";
}

} // namespace exchange::bench
