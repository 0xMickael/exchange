#pragma once

#include "exchange/core/event.hpp"
#include "exchange/core/text.hpp"
#include "exchange/core/types.hpp"

#include <string>
#include <vector>

// Short constructors for expected events, so that a test reads like the event
// tables of SPEC section 5.4.
namespace exchange::test {

inline Event accepted(SessionId session, ClientOrderId client_order_id, OrderId order) {
    return OrderAccepted{
        .session_id = session, .client_order_id = client_order_id, .order_id = order};
}

inline Event rejected(SessionId session, ClientOrderId client_order_id, RejectReason reason) {
    return OrderRejected{
        .session_id = session, .client_order_id = client_order_id, .reason = reason};
}

inline Event fill(SessionId session, OrderId order, Price price, Quantity quantity,
                  Quantity remaining) {
    return Fill{.session_id = session,
                .order_id = order,
                .price = price,
                .quantity = quantity,
                .remaining_quantity = remaining};
}

inline Event cancelled(SessionId session, OrderId order, Quantity quantity,
                       CancelReason reason = CancelReason::Requested) {
    return OrderCancelled{
        .session_id = session, .order_id = order, .cancelled_quantity = quantity, .reason = reason};
}

inline Event cancel_rejected(SessionId session, OrderId order) {
    return CancelRejected{
        .session_id = session, .order_id = order, .reason = CancelRejectReason::UnknownOrder};
}

inline Event trade(Price price, Quantity quantity, Side aggressor) {
    return Trade{.price = price, .quantity = quantity, .aggressor_side = aggressor};
}

inline Event book(Side side, Price price, Quantity total) {
    return BookUpdate{.side = side, .price = price, .total_quantity = total};
}

// Events as text lines. Comparing these instead of the events themselves makes
// a failing test print a readable difference.
inline std::vector<std::string> text(const std::vector<Event>& events) {
    std::vector<std::string> lines;
    lines.reserve(events.size());
    for (const Event& event : events) {
        lines.push_back(to_string(event));
    }
    return lines;
}

} // namespace exchange::test
