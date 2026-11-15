#pragma once

#include "exchange/core/event.hpp"
#include "exchange/core/types.hpp"
#include "exchange/protocol/messages.hpp"

#include <cstdint>
#include <iterator>
#include <set>
#include <variant>

namespace exchange::server {

// Turns the engine's events into protocol messages and decides which sessions
// receive each of them (SPEC section 6).
//
//   - A private event goes to the session it names.
//   - A public event (Trade, BookUpdate) gets the next market data sequence
//     number and goes to every subscribed session.
//   - A session becomes subscribed when its SnapshotEnd is published, so that
//     it receives exactly the updates that are newer than its snapshot.
//
// The class knows nothing about sockets. It hands each message to a `send`
// function: bool send(SessionId, const protocol::ServerMessage&), which
// returns false if the session can no longer receive anything.
class Publisher {
public:
    template <typename Send> void publish(const Event& event, Send&& send) {
        std::visit([this, &send](const auto& typed_event) { this->deliver(typed_event, send); },
                   event);
    }

    // Forgets a session that has closed.
    void remove_session(SessionId session_id) { subscribers_.erase(session_id); }

    [[nodiscard]] std::uint64_t last_sequence() const noexcept { return sequence_; }

private:
    template <typename Send> void deliver(const OrderAccepted& event, Send& send) {
        send(event.session_id, protocol::OrderAccepted{.client_order_id = event.client_order_id,
                                                       .order_id = event.order_id});
    }

    template <typename Send> void deliver(const OrderRejected& event, Send& send) {
        send(event.session_id, protocol::OrderRejected{.client_order_id = event.client_order_id,
                                                       .reason = event.reason});
    }

    template <typename Send> void deliver(const Fill& event, Send& send) {
        send(event.session_id, protocol::Fill{.order_id = event.order_id,
                                              .price = event.price,
                                              .quantity = event.quantity,
                                              .remaining_quantity = event.remaining_quantity});
    }

    template <typename Send> void deliver(const OrderCancelled& event, Send& send) {
        send(event.session_id,
             protocol::OrderCancelled{.order_id = event.order_id,
                                      .cancelled_quantity = event.cancelled_quantity,
                                      .reason = event.reason});
    }

    template <typename Send> void deliver(const CancelRejected& event, Send& send) {
        send(event.session_id,
             protocol::CancelRejected{.order_id = event.order_id, .reason = event.reason});
    }

    template <typename Send> void deliver(const SnapshotBegin& event, Send& send) {
        send(event.session_id, protocol::SnapshotBegin{.last_sequence = sequence_});
    }

    template <typename Send> void deliver(const SnapshotLevel& event, Send& send) {
        send(event.session_id, protocol::SnapshotLevel{.side = event.side,
                                                       .price = event.price,
                                                       .total_quantity = event.total_quantity});
    }

    template <typename Send> void deliver(const SnapshotEnd& event, Send& send) {
        if (send(event.session_id, protocol::SnapshotEnd{})) {
            subscribers_.insert(event.session_id);
        }
    }

    template <typename Send> void deliver(const Trade& event, Send& send) {
        ++sequence_;
        broadcast(protocol::Trade{.sequence = sequence_,
                                  .price = event.price,
                                  .quantity = event.quantity,
                                  .aggressor_side = event.aggressor_side},
                  send);
    }

    template <typename Send> void deliver(const BookUpdate& event, Send& send) {
        ++sequence_;
        broadcast(protocol::BookUpdate{.sequence = sequence_,
                                       .side = event.side,
                                       .price = event.price,
                                       .total_quantity = event.total_quantity},
                  send);
    }

    template <typename Send> void broadcast(const protocol::ServerMessage& message, Send& send) {
        for (auto it = subscribers_.begin(); it != subscribers_.end();) {
            // A subscriber that cannot receive is dropped: it would otherwise
            // see a gap in the sequence numbers.
            it = send(*it, message) ? std::next(it) : subscribers_.erase(it);
        }
    }

    std::uint64_t sequence_ = 0;
    // Ordered, so that subscribers are served in the same order on every run.
    std::set<SessionId> subscribers_;
};

} // namespace exchange::server
