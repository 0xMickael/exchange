#pragma once

#include "exchange/core/types.hpp"

#include <cstddef>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace exchange {

// An order waiting in the book.
struct Order {
    OrderId id;
    SessionId session_id;
    Side side;
    Price price;
    Quantity quantity;  // Original quantity.
    Quantity remaining; // Quantity not yet filled or cancelled.
};

struct LevelInfo {
    Price price;
    Quantity total_quantity;

    friend bool operator==(const LevelInfo&, const LevelInfo&) = default;
};

// The resting orders of both sides, organized for price-time priority.
//
// The book is a passive container: it stores orders and keeps them in priority
// order, but decides nothing. Validation, matching rules and events belong to
// the MatchingEngine.
//
// Structure:
//   - one ordered map per side, from price to level;
//   - each level holds its orders in a FIFO list, oldest first;
//   - an index from OrderId to the order's position, so that a cancel does not
//     have to search the book.
class OrderBook {
public:
    // The order with the highest priority on `side`, or nullptr if the side is empty.
    // The pointer is invalidated by the next call that modifies the book.
    [[nodiscard]] const Order* best(Side side) const;

    // The active order with this id, or nullptr.
    [[nodiscard]] const Order* find(OrderId id) const;

    // Total remaining quantity at a price, or 0 if there is no such level.
    [[nodiscard]] Quantity level_quantity(Side side, Price price) const;

    // Adds an order at the back of its level. Precondition: its id is not in the
    // book and its remaining quantity is positive.
    void add(const Order& order);

    // Reduces the best order of `side` by `quantity` and removes it once it is
    // fully filled. Precondition: best(side) exists and has at least `quantity` remaining.
    void fill_best(Side side, Quantity quantity);

    // Removes an order with whatever quantity it has left. Precondition: find(id) exists.
    void remove(OrderId id);

    // All levels of a side, best price first.
    [[nodiscard]] std::vector<LevelInfo> levels(Side side) const;

    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }

    // Verifies invariants I1 to I5 of SPEC section 5.6. Returns an empty string
    // if they hold, otherwise a description of the first violation. Costs a full
    // traversal of the book: for tests, not for the hot path.
    [[nodiscard]] std::string check_invariants() const;

private:
    struct Level {
        Quantity total_quantity = 0;
        std::list<Order> orders;
    };
    using Levels = std::map<Price, Level>;

    [[nodiscard]] Levels& levels_of(Side side) noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }
    [[nodiscard]] const Levels& levels_of(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    [[nodiscard]] std::string check_level(Side side, Price price, const Level& level) const;

    // Both maps are sorted by increasing price, so the best bid is the last
    // entry of bids_ and the best ask is the first entry of asks_.
    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderId, std::list<Order>::iterator> index_;
};

} // namespace exchange
