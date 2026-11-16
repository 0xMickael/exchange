#include "exchange/core/matching_engine.hpp"

#include <algorithm>
#include <cassert>
#include <optional>

namespace exchange {

namespace {

// Rule M1: can an incoming order at `limit` trade with a resting order at `resting_price`?
bool crosses(Side incoming_side, Price limit, Price resting_price) noexcept {
    return incoming_side == Side::Buy ? resting_price <= limit : resting_price >= limit;
}

} // namespace

void MatchingEngine::process(const Command& command, std::vector<Event>& events) {
    std::visit([this, &events](const auto& typed_command) { handle(typed_command, events); },
               command);
}

void MatchingEngine::handle(const NewOrder& command, std::vector<Event>& events) {
    Session& session = sessions_[command.session_id];

    const auto reject = [&](RejectReason reason) {
        events.emplace_back(OrderRejected{.session_id = command.session_id,
                                          .client_order_id = command.client_order_id,
                                          .reason = reason});
    };

    // Validation, in the order fixed by SPEC section 4.1.
    if (command.client_order_id <= session.last_client_order_id) {
        reject(RejectReason::InvalidClientOrderId);
        return;
    }
    session.last_client_order_id = command.client_order_id;
    if (!is_valid_price(command.price)) {
        reject(RejectReason::InvalidPrice);
        return;
    }
    if (!is_valid_quantity(command.quantity)) {
        reject(RejectReason::InvalidQuantity);
        return;
    }

    const OrderId order_id = next_order_id_++;
    events.emplace_back(OrderAccepted{.session_id = command.session_id,
                                      .client_order_id = command.client_order_id,
                                      .order_id = order_id});

    Quantity remaining = command.quantity;
    match(command, order_id, remaining, events);

    // Rule M7: whatever is left rests in the book.
    if (remaining > 0) {
        const Quantity level_total = book_.add(Order{.id = order_id,
                                                     .session_id = command.session_id,
                                                     .side = command.side,
                                                     .price = command.price,
                                                     .quantity = command.quantity,
                                                     .remaining = remaining});
        events.emplace_back(BookUpdate{
            .side = command.side, .price = command.price, .total_quantity = level_total});
    }
}

void MatchingEngine::match(const NewOrder& command, OrderId order_id, Quantity& remaining,
                           std::vector<Event>& events) {
    const Side resting_side = opposite(command.side);

    // One BookUpdate is emitted per level, after the last trade at that level.
    std::optional<Price> current_level;
    const auto finish_level = [&] {
        if (current_level.has_value()) {
            events.emplace_back(
                BookUpdate{.side = resting_side,
                           .price = *current_level,
                           .total_quantity = book_.level_quantity(resting_side, *current_level)});
        }
    };

    while (remaining > 0) {
        // best() applies rules M2 and M3: best price first, oldest order first.
        const Order* resting = book_.best(resting_side);
        if (resting == nullptr || !crosses(command.side, command.price, resting->price)) {
            break;
        }
        if (current_level != resting->price) {
            finish_level();
            current_level = resting->price;
        }

        // Rules M4 and M5: the resting order's price, the smaller remaining quantity.
        const Price price = resting->price;
        const Quantity quantity = std::min(remaining, resting->remaining);
        const SessionId maker_session = resting->session_id;
        const OrderId maker_order = resting->id;
        const Quantity maker_remaining = resting->remaining - quantity;
        remaining -= quantity;

        book_.fill_best(resting_side, quantity); // Invalidates `resting`.

        events.emplace_back(Fill{.session_id = maker_session,
                                 .order_id = maker_order,
                                 .price = price,
                                 .quantity = quantity,
                                 .remaining_quantity = maker_remaining});
        events.emplace_back(Fill{.session_id = command.session_id,
                                 .order_id = order_id,
                                 .price = price,
                                 .quantity = quantity,
                                 .remaining_quantity = remaining});
        events.emplace_back(
            Trade{.price = price, .quantity = quantity, .aggressor_side = command.side});
    }
    finish_level();
}

void MatchingEngine::handle(const CancelOrder& command, std::vector<Event>& events) {
    const Order* order = book_.find(command.order_id);
    // An order of another session is reported exactly like an unknown one (SPEC section 4.2).
    if (order == nullptr || order->session_id != command.session_id) {
        events.emplace_back(CancelRejected{.session_id = command.session_id,
                                           .order_id = command.order_id,
                                           .reason = CancelRejectReason::UnknownOrder});
        return;
    }
    cancel(*order, CancelReason::Requested, events);
}

void MatchingEngine::handle(const SessionClosed& command, std::vector<Event>& events) {
    // The book lists a session's orders from oldest to newest. Order ids are
    // assigned in increasing order, so this is also increasing OrderId order,
    // as SPEC section 4.3 requires.
    for (const OrderId order_id : book_.orders_of(command.session_id)) {
        const Order* order = book_.find(order_id);
        assert(order != nullptr);
        cancel(*order, CancelReason::SessionClosed, events);
    }
    sessions_.erase(command.session_id);
}

void MatchingEngine::handle(const SnapshotRequest& command, std::vector<Event>& events) const {
    events.emplace_back(SnapshotBegin{.session_id = command.session_id});
    for (const Side side : {Side::Buy, Side::Sell}) {
        for (const LevelInfo& level : book_.levels(side)) {
            events.emplace_back(SnapshotLevel{.session_id = command.session_id,
                                              .side = side,
                                              .price = level.price,
                                              .total_quantity = level.total_quantity});
        }
    }
    events.emplace_back(SnapshotEnd{.session_id = command.session_id});
}

void MatchingEngine::cancel(const Order& order, CancelReason reason, std::vector<Event>& events) {
    // Copied first: removing the order from the book destroys it.
    const OrderId order_id = order.id;
    const SessionId session_id = order.session_id;
    const Side side = order.side;
    const Price price = order.price;
    const Quantity cancelled_quantity = order.remaining;

    const Quantity level_total = book_.remove(order_id);

    events.emplace_back(OrderCancelled{.session_id = session_id,
                                       .order_id = order_id,
                                       .cancelled_quantity = cancelled_quantity,
                                       .reason = reason});
    events.emplace_back(BookUpdate{.side = side, .price = price, .total_quantity = level_total});
}

std::string MatchingEngine::check_invariants() const {
    return book_.check_invariants();
}

} // namespace exchange
