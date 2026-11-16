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
    return &best_level(levels, side)->second.orders.front().order;
}

const Order* OrderBook::find(OrderId id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &it->second->order;
}

Quantity OrderBook::level_quantity(Side side, Price price) const {
    const Levels& levels = levels_of(side);
    const auto it = levels.find(price);
    return it == levels.end() ? 0 : it->second.total_quantity;
}

Quantity OrderBook::add(const Order& order) {
    assert(order.remaining > 0);
    assert(!index_.contains(order.id));

    Level& level = levels_of(order.side)[order.price];
    SessionOrders& session = sessions_[order.session_id];
    Node& node = level.orders.emplace_back(
        Node{.order = order, .previous_of_session = session.newest, .next_of_session = nullptr});
    level.total_quantity += order.remaining;
    index_.emplace(order.id, std::prev(level.orders.end()));

    if (session.newest != nullptr) {
        session.newest->next_of_session = &node;
    } else {
        session.oldest = &node;
    }
    session.newest = &node;
    return level.total_quantity;
}

std::vector<OrderId> OrderBook::orders_of(SessionId session_id) const {
    std::vector<OrderId> result;
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return result;
    }
    for (const Node* node = it->second.oldest; node != nullptr; node = node->next_of_session) {
        result.push_back(node->order.id);
    }
    return result;
}

void OrderBook::unlink_from_session(Node& node) {
    Node* const previous = node.previous_of_session;
    Node* const next = node.next_of_session;
    if (previous != nullptr && next != nullptr) {
        // In the middle of the chain: only the two neighbors change.
        previous->next_of_session = next;
        next->previous_of_session = previous;
        return;
    }

    // At one end of the chain, or alone in it: the session's entry changes too.
    const auto it = sessions_.find(node.order.session_id);
    assert(it != sessions_.end());
    SessionOrders& session = it->second;
    if (previous != nullptr) {
        previous->next_of_session = next;
    } else {
        session.oldest = next;
    }
    if (next != nullptr) {
        next->previous_of_session = previous;
    } else {
        session.newest = previous;
    }
    if (session.oldest == nullptr) {
        sessions_.erase(it);
    }
}

void OrderBook::fill_best(Side side, Quantity quantity) {
    Levels& levels = levels_of(side);
    assert(!levels.empty());

    const auto level_it = best_level(levels, side);
    Level& level = level_it->second;
    Node& node = level.orders.front();
    Order& order = node.order;
    assert(quantity > 0 && quantity <= order.remaining);

    order.remaining -= quantity;
    level.total_quantity -= quantity;
    if (order.remaining == 0) {
        unlink_from_session(node);
        index_.erase(order.id);
        level.orders.pop_front();
        if (level.orders.empty()) {
            levels.erase(level_it);
        }
    }
}

Quantity OrderBook::remove(OrderId id) {
    const auto index_it = index_.find(id);
    assert(index_it != index_.end());
    const auto node_it = index_it->second;
    const Order& order = node_it->order;

    Levels& levels = levels_of(order.side);
    const auto level_it = levels.find(order.price);
    assert(level_it != levels.end());
    Level& level = level_it->second;

    level.total_quantity -= order.remaining;
    const Quantity left = level.total_quantity;
    unlink_from_session(*node_it);
    level.orders.erase(node_it);
    if (level.orders.empty()) {
        levels.erase(level_it);
    }
    index_.erase(index_it);
    return left;
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
        const Order& order = it->order;
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

std::string OrderBook::check_sessions() const {
    std::size_t orders_seen = 0;
    for (const auto& [session_id, session] : sessions_) {
        const std::string name = "session " + std::to_string(session_id);
        if (session.oldest == nullptr) {
            return name + " is listed with no orders (I6)";
        }
        const Node* previous = nullptr;
        for (const Node* node = session.oldest; node != nullptr; node = node->next_of_session) {
            const Order& order = node->order;
            const auto index_it = index_.find(order.id);
            if (index_it == index_.end() || &*index_it->second != node) {
                return name + " lists an order that is not in the book (I6)";
            }
            if (order.session_id != session_id) {
                return name + " lists order " + std::to_string(order.id) +
                       ", which belongs to another session (I6)";
            }
            if (node->previous_of_session != previous) {
                return name + " has a broken chain at order " + std::to_string(order.id) + " (I6)";
            }
            if (previous != nullptr && order.id <= previous->order.id) {
                return name + " lists order " + std::to_string(order.id) + " out of order (I6)";
            }
            previous = node;
            ++orders_seen;
        }
        if (session.newest != previous) {
            return name + " has a wrong newest order (I6)";
        }
    }
    if (orders_seen != index_.size()) {
        return "some active orders are not listed by their session (I6)";
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
    std::string violation = check_sessions();
    if (!violation.empty()) {
        return violation;
    }
    if (!bids_.empty() && !asks_.empty() && bids_.rbegin()->first >= asks_.begin()->first) {
        return "the book is crossed (I1)";
    }
    return {};
}

} // namespace exchange
