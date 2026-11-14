#include "exchange/core/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace exchange {

namespace {

// The level with the best price. Precondition: `levels` is not empty.
template <typename LevelsT> auto best_level(LevelsT& levels, Side side) {
    return side == Side::Buy ? std::prev(levels.end()) : levels.begin();
}

std::string describe(Side side, Price price) {
    return std::string(side == Side::Buy ? "bid" : "ask") + " level " + std::to_string(price);
}

} // namespace

const Order* OrderBook::best(Side side) const {
    const Levels& levels = levels_of(side);
    if (levels.empty()) {
        return nullptr;
    }
    return &best_level(levels, side)->second.orders.front();
}

const Order* OrderBook::find(OrderId id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &*it->second;
}

Quantity OrderBook::level_quantity(Side side, Price price) const {
    const Levels& levels = levels_of(side);
    const auto it = levels.find(price);
    return it == levels.end() ? 0 : it->second.total_quantity;
}

void OrderBook::add(const Order& order) {
    assert(order.remaining > 0);
    assert(!index_.contains(order.id));

    Level& level = levels_of(order.side)[order.price];
    level.orders.push_back(order);
    level.total_quantity += order.remaining;
    index_.emplace(order.id, std::prev(level.orders.end()));
}

void OrderBook::fill_best(Side side, Quantity quantity) {
    Levels& levels = levels_of(side);
    assert(!levels.empty());

    const auto level_it = best_level(levels, side);
    Level& level = level_it->second;
    Order& order = level.orders.front();
    assert(quantity > 0 && quantity <= order.remaining);

    order.remaining -= quantity;
    level.total_quantity -= quantity;
    if (order.remaining == 0) {
        index_.erase(order.id);
        level.orders.pop_front();
        if (level.orders.empty()) {
            levels.erase(level_it);
        }
    }
}

void OrderBook::remove(OrderId id) {
    const auto index_it = index_.find(id);
    assert(index_it != index_.end());
    const auto order_it = index_it->second;

    Levels& levels = levels_of(order_it->side);
    const auto level_it = levels.find(order_it->price);
    assert(level_it != levels.end());
    Level& level = level_it->second;

    level.total_quantity -= order_it->remaining;
    level.orders.erase(order_it);
    if (level.orders.empty()) {
        levels.erase(level_it);
    }
    index_.erase(index_it);
}

std::vector<LevelInfo> OrderBook::levels(Side side) const {
    const Levels& levels = levels_of(side);
    std::vector<LevelInfo> result;
    result.reserve(levels.size());
    for (const auto& [price, level] : levels) {
        result.push_back(LevelInfo{.price = price, .total_quantity = level.total_quantity});
    }
    if (side == Side::Buy) {
        std::ranges::reverse(result);
    }
    return result;
}

std::string OrderBook::check_level(Side side, Price price, const Level& level) const {
    if (level.orders.empty()) {
        return describe(side, price) + " is empty (I3)";
    }
    Quantity sum = 0;
    OrderId previous_id = 0;
    for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
        const Order& order = *it;
        const std::string name = "order " + std::to_string(order.id);
        if (order.side != side || order.price != price) {
            return name + " is stored in the wrong level: " + describe(side, price);
        }
        if (order.remaining == 0 || order.remaining > order.quantity) {
            return name + " has an invalid remaining quantity (I2)";
        }
        if (order.id <= previous_id) {
            return name + " breaks time priority in " + describe(side, price) + " (I4)";
        }
        const auto index_it = index_.find(order.id);
        if (index_it == index_.end() || index_it->second != it) {
            return name + " is missing from the cancel index (I5)";
        }
        previous_id = order.id;
        sum += order.remaining;
    }
    if (sum != level.total_quantity) {
        return describe(side, price) + " has a wrong total (I3)";
    }
    return {};
}

std::string OrderBook::check_invariants() const {
    std::size_t orders_seen = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
        for (const auto& [price, level] : levels_of(side)) {
            std::string violation = check_level(side, price, level);
            if (!violation.empty()) {
                return violation;
            }
            orders_seen += level.orders.size();
        }
    }

    if (orders_seen != index_.size()) {
        return "the cancel index contains orders that are not in the book (I5)";
    }
    if (!bids_.empty() && !asks_.empty() && bids_.rbegin()->first >= asks_.begin()->first) {
        return "the book is crossed (I1)";
    }
    return {};
}

} // namespace exchange
