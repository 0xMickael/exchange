#pragma once

#include "exchange/core/command.hpp"
#include "exchange/core/event.hpp"
#include "exchange/core/order_book.hpp"
#include "exchange/core/types.hpp"

#include <map>
#include <vector>

namespace exchange::test {

// A second implementation of SPEC sections 4 and 5, written to be obviously
// correct instead of fast. It keeps every resting order in one flat vector and
// answers every question by scanning it.
//
// It shares no code with MatchingEngine, so the two can only agree on a random
// command stream if both follow the specification.
class ReferenceEngine {
public:
    void process(const Command& command, std::vector<Event>& events);

    // All levels of a side, best price first.
    [[nodiscard]] std::vector<LevelInfo> levels(Side side) const;

private:
    struct RestingOrder {
        OrderId id;
        SessionId session_id;
        Side side;
        Price price;
        Quantity remaining;
    };

    void new_order(const NewOrder& command, std::vector<Event>& events);
    void cancel_order(const CancelOrder& command, std::vector<Event>& events);
    void close_session(const SessionClosed& command, std::vector<Event>& events);

    [[nodiscard]] Quantity level_total(Side side, Price price) const;
    // The resting order that an incoming order must trade with next, or
    // orders_.end() if nothing crosses.
    [[nodiscard]] std::vector<RestingOrder>::iterator next_match(Side incoming_side, Price limit);

    std::vector<RestingOrder> orders_; // In acceptance order, so in increasing OrderId order.
    std::map<SessionId, ClientOrderId> last_client_order_id_;
    OrderId next_order_id_ = 1;
};

} // namespace exchange::test
