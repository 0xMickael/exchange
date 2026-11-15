#pragma once

#include "exchange/core/command.hpp"
#include "exchange/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace exchange::bench {

// Parameters of a synthetic order flow around a fixed mid price (SPEC section 13.2).
struct WorkloadConfig {
    std::uint64_t seed = 42;
    // Passive orders placed before measurement starts, to give the book depth.
    std::size_t initial_orders = 10'000;
    // Number of measured commands.
    std::size_t commands = 1'000'000;
    std::uint32_t sessions = 8;
    // Passive orders are priced 1 to price_levels ticks away from the mid price.
    std::uint32_t price_levels = 50;
    // Share of measured commands that cancel a live order.
    std::uint32_t cancel_percent = 40;
    // Share of measured commands that are orders priced to trade immediately.
    // The rest are passive orders.
    std::uint32_t aggressive_percent = 10;
    Price mid_price = 100'000;
    Quantity max_quantity = 100;
};

// What the measured commands did, so that a result can be interpreted.
struct WorkloadStats {
    std::uint64_t passive_orders = 0;
    std::uint64_t aggressive_orders = 0;
    std::uint64_t cancels = 0;
    std::uint64_t trades = 0;
    std::uint64_t events = 0;
    std::uint64_t final_orders = 0;
    std::uint64_t final_bid_levels = 0;
    std::uint64_t final_ask_levels = 0;
};

struct Workload {
    std::vector<Command> setup;    // Applied before measurement.
    std::vector<Command> measured; // The commands that are timed.
    WorkloadStats stats;
};

// Returns an empty string if the configuration is usable, otherwise the reason it is not.
[[nodiscard]] std::string validate(const WorkloadConfig& config);

// Generates the same workload for the same configuration on every platform.
// Every generated command is valid, and every cancel targets an order that is
// live at that point of the stream. Precondition: validate(config) is empty.
[[nodiscard]] Workload generate_workload(const WorkloadConfig& config);

} // namespace exchange::bench
