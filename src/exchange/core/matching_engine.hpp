#pragma once

#include "exchange/core/command.hpp"
#include "exchange/core/event.hpp"
#include "exchange/core/order_book.hpp"
#include "exchange/core/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace exchange {

// Applies commands to the order book according to SPEC sections 4 and 5.
//
// The engine is single-threaded and has no I/O and no clock: the events it
// emits depend only on the sequence of commands it has received.
class MatchingEngine {
public:
    // Processes one command and appends the resulting events to `events`, in the
    // order defined by SPEC section 5.3. `events` is not cleared, so a caller can
    // reuse one vector across calls and avoid repeated allocations.
    void process(const Command& command, std::vector<Event>& events);

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

    // Verifies the invariants of SPEC section 5.6 that can be checked on the
    // engine's state (I1 to I6). Returns an empty string if they hold. For tests.
    [[nodiscard]] std::string check_invariants() const;

private:
    // What the engine remembers about a session. Its active orders are not
    // listed here: the book knows them (OrderBook::orders_of).
    struct Session {
        ClientOrderId last_client_order_id = 0;
    };

    void handle(const NewOrder& command, std::vector<Event>& events);
    void handle(const CancelOrder& command, std::vector<Event>& events);
    void handle(const SessionClosed& command, std::vector<Event>& events);
    void handle(const SnapshotRequest& command, std::vector<Event>& events) const;

    void match(const NewOrder& command, OrderId order_id, Quantity& remaining,
               std::vector<Event>& events);
    // Removes an order of the book and emits the events of its cancellation.
    void cancel(const Order& order, CancelReason reason, std::vector<Event>& events);

    OrderBook book_;
    std::unordered_map<SessionId, Session> sessions_;
    OrderId next_order_id_ = 1;
};

} // namespace exchange
