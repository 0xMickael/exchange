#include "exchange/core/matching_engine.hpp"
#include "exchange/core/text.hpp"

#include "support/events.hpp"
#include "support/random_commands.hpp"
#include "support/reference_engine.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

// Runs random command streams through MatchingEngine and through the naive
// ReferenceEngine, and requires identical events after every command.

namespace exchange {
namespace {

using test::RandomCommands;
using test::ReferenceEngine;
using test::WorkloadShape;

// What the event stream has said about one order so far.
struct OrderHistory {
    Quantity original = 0;
    Quantity filled = 0;
    Quantity cancelled = 0;
};

using Ledger = std::map<OrderId, OrderHistory>;

// Checks properties of the events of one command that must hold whatever the
// command was: invariant I7 and the quantity accounting of SPEC section 3.
void check_events(const Command& command, const std::vector<Event>& events, Ledger& ledger) {
    for (std::size_t i = 0; i < events.size(); ++i) {
        const Event& event = events[i];

        if (const auto* accepted = std::get_if<OrderAccepted>(&event)) {
            ASSERT_TRUE(std::holds_alternative<NewOrder>(command));
            ASSERT_FALSE(ledger.contains(accepted->order_id)) << "OrderId assigned twice";
            ledger[accepted->order_id].original = std::get<NewOrder>(command).quantity;
        } else if (const auto* fill = std::get_if<Fill>(&event)) {
            ASSERT_TRUE(ledger.contains(fill->order_id));
            OrderHistory& history = ledger[fill->order_id];
            history.filled += fill->quantity;
            ASSERT_EQ(history.cancelled, 0U) << "fill after cancel";
            ASSERT_LE(history.filled, history.original);
            ASSERT_EQ(fill->remaining_quantity, history.original - history.filled);
        } else if (const auto* cancelled = std::get_if<OrderCancelled>(&event)) {
            ASSERT_TRUE(ledger.contains(cancelled->order_id));
            OrderHistory& history = ledger[cancelled->order_id];
            ASSERT_EQ(history.cancelled, 0U) << "order cancelled twice";
            ASSERT_GT(cancelled->cancelled_quantity, 0U);
            ASSERT_EQ(cancelled->cancelled_quantity, history.original - history.filled);
            history.cancelled = cancelled->cancelled_quantity;
        } else if (const auto* trade = std::get_if<Trade>(&event)) {
            // I7: a Trade follows two Fills with its price and quantity.
            ASSERT_GE(i, 2U);
            for (const std::size_t fill_index : {i - 2, i - 1}) {
                const auto* trade_fill = std::get_if<Fill>(&events[fill_index]);
                ASSERT_NE(trade_fill, nullptr);
                ASSERT_EQ(trade_fill->price, trade->price);
                ASSERT_EQ(trade_fill->quantity, trade->quantity);
            }
        }
    }
}

// Every order's quantity is fully accounted for: filled, cancelled, or still in the book.
void check_ledger(const Ledger& ledger, const OrderBook& book) {
    std::size_t active_orders = 0;
    for (const auto& [order_id, history] : ledger) {
        const Order* order = book.find(order_id);
        const Quantity remaining = order == nullptr ? 0 : order->remaining;
        ASSERT_EQ(history.filled + history.cancelled + remaining, history.original)
            << "order " << order_id;
        active_orders += order == nullptr ? 0U : 1U;
    }
    ASSERT_EQ(active_orders, book.order_count());
}

struct Scenario {
    std::string name;
    WorkloadShape shape;
};

// Makes test listings show the scenario name instead of a dump of its bytes.
void PrintTo(const Scenario& scenario, std::ostream* os) { // NOLINT(readability-identifier-naming)
    *os << scenario.name;
}

class DifferentialTest : public ::testing::TestWithParam<Scenario> {};

TEST_P(DifferentialTest, EngineMatchesReferenceOnRandomCommands) {
    constexpr std::uint64_t seeds = 100;
    constexpr int commands_per_seed = 1000;

    std::size_t trades = 0;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        RandomCommands commands(seed, GetParam().shape);
        MatchingEngine engine;
        ReferenceEngine reference;
        Ledger ledger;
        std::vector<Event> engine_events;
        std::vector<Event> reference_events;

        for (int i = 0; i < commands_per_seed; ++i) {
            const Command command = commands.next();
            // Built only when an assertion fails: gtest evaluates what is
            // streamed into an assertion only on failure.
            const auto context = [&] {
                return "seed " + std::to_string(seed) + ", command " + std::to_string(i) + ": " +
                       to_string(command);
            };

            engine_events.clear();
            reference_events.clear();
            engine.process(command, engine_events);
            reference.process(command, reference_events);

            if (engine_events != reference_events) {
                // Compared again as text to get a readable difference.
                ASSERT_EQ(test::text(engine_events), test::text(reference_events)) << context();
            }
            ASSERT_EQ(engine.check_invariants(), "") << context();
            ASSERT_NO_FATAL_FAILURE(check_events(command, engine_events, ledger)) << context();
            for (const Event& event : engine_events) {
                trades += std::holds_alternative<Trade>(event) ? 1U : 0U;
            }
        }

        SCOPED_TRACE("seed " + std::to_string(seed) + ", final state");
        ASSERT_EQ(engine.book().levels(Side::Buy), reference.levels(Side::Buy));
        ASSERT_EQ(engine.book().levels(Side::Sell), reference.levels(Side::Sell));
        ASSERT_NO_FATAL_FAILURE(check_ledger(ledger, engine.book()));
    }

    // Guards against a generator that would never make orders cross.
    EXPECT_GT(trades, seeds * 10);
}

INSTANTIATE_TEST_SUITE_P(
    Workloads, DifferentialTest,
    ::testing::Values(
        Scenario{.name = "SinglePrice",
                 .shape = {.lowest_price = 100, .highest_price = 100, .max_quantity = 5}},
        Scenario{.name = "NarrowBand",
                 .shape = {.lowest_price = 95, .highest_price = 105, .max_quantity = 20}},
        Scenario{.name = "WideBand",
                 .shape = {.lowest_price = 1, .highest_price = 2000, .max_quantity = 1000}}),
    [](const ::testing::TestParamInfo<Scenario>& scenario) { return scenario.param.name; });

} // namespace
} // namespace exchange
