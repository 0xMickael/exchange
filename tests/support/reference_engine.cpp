#include "support/reference_engine.hpp"

#include <algorithm>
#include <functional>
#include <variant>

namespace exchange::test {

namespace {

struct TradeRecord {
    SessionId maker_session;
    OrderId maker_order;
    Price price;
    Quantity quantity;
    Quantity maker_remaining;
    Quantity taker_remaining;
};

} // namespace

void ReferenceEngine::process(const Command& command, std::vector<Event>& events) {
    if (const auto* new_order_command = std::get_if<NewOrder>(&command)) {
        new_order(*new_order_command, events);
    } else if (const auto* cancel_command = std::get_if<CancelOrder>(&command)) {
        cancel_order(*cancel_command, events);
    } else {
        close_session(std::get<SessionClosed>(command), events);
    }
}

Quantity ReferenceEngine::level_total(Side side, Price price) const {
    Quantity total = 0;
    for (const RestingOrder& order : orders_) {
        if (order.side == side && order.price == price) {
            total += order.remaining;
        }
    }
    return total;
}

std::vector<ReferenceEngine::RestingOrder>::iterator ReferenceEngine::next_match(Side incoming_side,
                                                                                 Price limit) {
    auto best = orders_.end();
    for (auto it = orders_.begin(); it != orders_.end(); ++it) {
        if (it->side == incoming_side || it->remaining == 0) {
            continue;
        }
        const bool crosses = incoming_side == Side::Buy ? it->price <= limit : it->price >= limit;
        if (!crosses) {
            continue;
        }
        // A strictly better price wins. At an equal price the earlier order,
        // which was seen first, is kept.
        const bool better =
            best == orders_.end() ||
            (incoming_side == Side::Buy ? it->price < best->price : it->price > best->price);
        if (better) {
            best = it;
        }
    }
    return best;
}

void ReferenceEngine::new_order(const NewOrder& command, std::vector<Event>& events) {
    const auto reject = [&](RejectReason reason) {
        events.emplace_back(OrderRejected{.session_id = command.session_id,
                                          .client_order_id = command.client_order_id,
                                          .reason = reason});
    };

    ClientOrderId& last_client_order_id = last_client_order_id_[command.session_id];
    if (command.client_order_id <= last_client_order_id) {
        reject(RejectReason::InvalidClientOrderId);
        return;
    }
    last_client_order_id = command.client_order_id;
    if (command.price < 1 || command.price > 1'000'000'000) {
        reject(RejectReason::InvalidPrice);
        return;
    }
    if (command.quantity < 1 || command.quantity > 1'000'000'000) {
        reject(RejectReason::InvalidQuantity);
        return;
    }

    const OrderId order_id = next_order_id_++;
    events.emplace_back(OrderAccepted{.session_id = command.session_id,
                                      .client_order_id = command.client_order_id,
                                      .order_id = order_id});

    // First decide every trade, then describe them.
    std::vector<TradeRecord> trades;
    Quantity remaining = command.quantity;
    while (remaining > 0) {
        const auto maker = next_match(command.side, command.price);
        if (maker == orders_.end()) {
            break;
        }
        const Quantity quantity = std::min(remaining, maker->remaining);
        maker->remaining -= quantity;
        remaining -= quantity;
        trades.push_back(TradeRecord{.maker_session = maker->session_id,
                                     .maker_order = maker->id,
                                     .price = maker->price,
                                     .quantity = quantity,
                                     .maker_remaining = maker->remaining,
                                     .taker_remaining = remaining});
    }
    std::erase_if(orders_, [](const RestingOrder& order) { return order.remaining == 0; });

    const Side resting_side = opposite(command.side);
    for (std::size_t i = 0; i < trades.size(); ++i) {
        const TradeRecord& trade = trades[i];
        events.emplace_back(Fill{.session_id = trade.maker_session,
                                 .order_id = trade.maker_order,
                                 .price = trade.price,
                                 .quantity = trade.quantity,
                                 .remaining_quantity = trade.maker_remaining});
        events.emplace_back(Fill{.session_id = command.session_id,
                                 .order_id = order_id,
                                 .price = trade.price,
                                 .quantity = trade.quantity,
                                 .remaining_quantity = trade.taker_remaining});
        events.emplace_back(Trade{
            .price = trade.price, .quantity = trade.quantity, .aggressor_side = command.side});

        const bool last_trade_of_level =
            i + 1 == trades.size() || trades[i + 1].price != trade.price;
        if (last_trade_of_level) {
            events.emplace_back(
                BookUpdate{.side = resting_side,
                           .price = trade.price,
                           .total_quantity = level_total(resting_side, trade.price)});
        }
    }

    if (remaining > 0) {
        orders_.push_back(RestingOrder{.id = order_id,
                                       .session_id = command.session_id,
                                       .side = command.side,
                                       .price = command.price,
                                       .remaining = remaining});
        events.emplace_back(BookUpdate{.side = command.side,
                                       .price = command.price,
                                       .total_quantity = level_total(command.side, command.price)});
    }
}

void ReferenceEngine::cancel_order(const CancelOrder& command, std::vector<Event>& events) {
    const auto it = std::ranges::find_if(orders_, [&](const RestingOrder& order) {
        return order.id == command.order_id && order.session_id == command.session_id;
    });
    if (it == orders_.end()) {
        events.emplace_back(CancelRejected{.session_id = command.session_id,
                                           .order_id = command.order_id,
                                           .reason = CancelRejectReason::UnknownOrder});
        return;
    }
    const RestingOrder order = *it;
    orders_.erase(it);
    events.emplace_back(OrderCancelled{.session_id = order.session_id,
                                       .order_id = order.id,
                                       .cancelled_quantity = order.remaining,
                                       .reason = CancelReason::Requested});
    events.emplace_back(BookUpdate{.side = order.side,
                                   .price = order.price,
                                   .total_quantity = level_total(order.side, order.price)});
}

void ReferenceEngine::close_session(const SessionClosed& command, std::vector<Event>& events) {
    while (true) {
        // orders_ is in increasing OrderId order, so the first match is the lowest id.
        const auto it = std::ranges::find(orders_, command.session_id, &RestingOrder::session_id);
        if (it == orders_.end()) {
            break;
        }
        const RestingOrder order = *it;
        orders_.erase(it);
        events.emplace_back(OrderCancelled{.session_id = order.session_id,
                                           .order_id = order.id,
                                           .cancelled_quantity = order.remaining,
                                           .reason = CancelReason::SessionClosed});
        events.emplace_back(BookUpdate{.side = order.side,
                                       .price = order.price,
                                       .total_quantity = level_total(order.side, order.price)});
    }
    last_client_order_id_.erase(command.session_id);
}

std::vector<LevelInfo> ReferenceEngine::levels(Side side) const {
    std::map<Price, Quantity> totals;
    for (const RestingOrder& order : orders_) {
        if (order.side == side) {
            totals[order.price] += order.remaining;
        }
    }
    std::vector<LevelInfo> result;
    result.reserve(totals.size());
    for (const auto& [price, total] : totals) {
        result.push_back(LevelInfo{.price = price, .total_quantity = total});
    }
    if (side == Side::Buy) {
        std::ranges::reverse(result);
    }
    return result;
}

} // namespace exchange::test
