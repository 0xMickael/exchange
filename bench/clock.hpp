#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#ifdef __x86_64__
#include <x86intrin.h>
#endif

namespace exchange::bench {

enum class ClockKind : std::uint8_t { Steady, Tsc };
enum class ClockChoice : std::uint8_t { Automatic, Steady, Tsc };

// The clock used to time single commands.
//
// std::chrono::steady_clock is portable, but its step can be much larger than
// the operation being timed: under WSL2 it advances in steps of 100 ns. The
// other source is the processor's timestamp counter (TSC), read with the RDTSC
// instruction: it counts at a fixed rate of a few ticks per nanosecond. Its
// rate is not known in advance, so it is calibrated against steady_clock.
//
// A reading is a number of ticks. Subtract two readings and convert the
// difference with to_nanoseconds().
class BenchClock {
public:
    [[nodiscard]] static BenchClock steady();
    // nullopt if the processor has no TSC or two calibrations disagree.
    [[nodiscard]] static std::optional<BenchClock> tsc();
    // nullopt only if the TSC was requested explicitly and is not usable.
    [[nodiscard]] static std::optional<BenchClock> create(ClockChoice choice);

    [[nodiscard]] std::uint64_t ticks() const noexcept {
#ifdef __x86_64__
        if (kind_ == ClockKind::Tsc) {
            // LFENCE makes the processor finish the instructions before it,
            // so the counter is not read while timed work is still running.
            _mm_lfence();
            return __rdtsc();
        }
#endif
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    [[nodiscard]] std::uint64_t to_nanoseconds(std::uint64_t tick_count) const noexcept {
        return static_cast<std::uint64_t>(
            std::llround(static_cast<double>(tick_count) * ns_per_tick_));
    }

    [[nodiscard]] ClockKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::string name() const;
    [[nodiscard]] double ns_per_tick() const noexcept { return ns_per_tick_; }
    // Smallest non-zero interval seen between two consecutive readings: nothing
    // shorter than this can be measured with this clock.
    [[nodiscard]] double smallest_interval_ns() const noexcept { return smallest_interval_ns_; }
    // Average cost of one reading. Every measured latency includes about one.
    [[nodiscard]] double reading_cost_ns() const noexcept { return reading_cost_ns_; }

private:
    BenchClock(ClockKind kind, double ns_per_tick);

    ClockKind kind_;
    double ns_per_tick_;
    double smallest_interval_ns_ = 0.0;
    double reading_cost_ns_ = 0.0;
};

} // namespace exchange::bench
