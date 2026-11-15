#pragma once

#include "exchange/core/command.hpp"
#include "exchange/core/types.hpp"

#include <array>
#include <cstdint>
#include <random>

namespace exchange::test {

// The range of prices and quantities that generated orders use. A narrow price
// range produces many trades; a wide one produces a deep book.
struct WorkloadShape {
    Price lowest_price;
    Price highest_price;
    Quantity max_quantity;
};

// Generates a reproducible stream of commands that exercises every rule of the
// specification, including invalid orders, cancels of orders that do not exist
// or belong to someone else, and session closes.
class RandomCommands {
public:
    RandomCommands(std::uint64_t seed, WorkloadShape shape) : rng_(seed), shape_(shape) {}

    Command next() {
        const std::uint64_t roll = below(100);
        if (roll < 3) {
            const SessionId session = random_session();
            last_client_order_id_.at(session) = 0;
            return SessionClosed{.session_id = session};
        }
        if (roll < 5) {
            return SnapshotRequest{.session_id = random_session()};
        }
        if (roll < 30) {
            // Covers live orders, dead orders and identifiers never assigned.
            return CancelOrder{.session_id = random_session(),
                               .order_id = below(new_orders_generated_ + 3)};
        }
        return random_new_order();
    }

private:
    static constexpr SessionId session_count = 3;

    // mt19937_64 produces the same sequence on every platform. The standard
    // distributions do not, so they are not used here.
    std::uint64_t below(std::uint64_t bound) { return rng_() % bound; }

    SessionId random_session() { return 1 + below(session_count); }

    Command random_new_order() {
        ++new_orders_generated_;
        const SessionId session = random_session();

        ClientOrderId& last = last_client_order_id_.at(session);
        const bool reuse_client_order_id = below(100) < 4;
        const ClientOrderId client_order_id = reuse_client_order_id ? last : ++last;

        const std::uint64_t price_roll = below(100);
        Price price = shape_.lowest_price + static_cast<Price>(below(static_cast<std::uint64_t>(
                                                shape_.highest_price - shape_.lowest_price + 1)));
        if (price_roll < 2) {
            price = 0;
        } else if (price_roll < 4) {
            price = max_price + 1;
        }

        const std::uint64_t quantity_roll = below(100);
        Quantity quantity = 1 + below(shape_.max_quantity);
        if (quantity_roll < 2) {
            quantity = 0;
        } else if (quantity_roll < 4) {
            quantity = max_quantity + 1;
        }

        return NewOrder{.session_id = session,
                        .client_order_id = client_order_id,
                        .side = below(2) == 0 ? Side::Buy : Side::Sell,
                        .price = price,
                        .quantity = quantity};
    }

    std::mt19937_64 rng_;
    WorkloadShape shape_;
    std::array<ClientOrderId, session_count + 1> last_client_order_id_{};
    std::uint64_t new_orders_generated_ = 0;
};

} // namespace exchange::test
