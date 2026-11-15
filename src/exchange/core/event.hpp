#pragma once

#include "exchange/core/types.hpp"

#include <cstdint>
#include <variant>

namespace exchange {

// Engine outputs (SPEC section 5.2).

enum class RejectReason : std::uint8_t { InvalidClientOrderId, InvalidPrice, InvalidQuantity };
enum class CancelRejectReason : std::uint8_t { UnknownOrder };
enum class CancelReason : std::uint8_t { Requested, SessionClosed };

// Private events: they concern one session.

struct OrderAccepted {
    SessionId session_id;
    ClientOrderId client_order_id;
    OrderId order_id;

    friend bool operator==(const OrderAccepted&, const OrderAccepted&) = default;
};

struct OrderRejected {
    SessionId session_id;
    ClientOrderId client_order_id;
    RejectReason reason;

    friend bool operator==(const OrderRejected&, const OrderRejected&) = default;
};

struct Fill {
    SessionId session_id;
    OrderId order_id;
    Price price;
    Quantity quantity;
    Quantity remaining_quantity;

    friend bool operator==(const Fill&, const Fill&) = default;
};

struct OrderCancelled {
    SessionId session_id;
    OrderId order_id;
    Quantity cancelled_quantity;
    CancelReason reason;

    friend bool operator==(const OrderCancelled&, const OrderCancelled&) = default;
};

struct CancelRejected {
    SessionId session_id;
    OrderId order_id;
    CancelRejectReason reason;

    friend bool operator==(const CancelRejected&, const CancelRejected&) = default;
};

// The answer to a SnapshotRequest: SnapshotBegin, one SnapshotLevel per price
// level, then SnapshotEnd.

struct SnapshotBegin {
    SessionId session_id;

    friend bool operator==(const SnapshotBegin&, const SnapshotBegin&) = default;
};

struct SnapshotLevel {
    SessionId session_id;
    Side side;
    Price price;
    Quantity total_quantity;

    friend bool operator==(const SnapshotLevel&, const SnapshotLevel&) = default;
};

struct SnapshotEnd {
    SessionId session_id;

    friend bool operator==(const SnapshotEnd&, const SnapshotEnd&) = default;
};

// Public events: they describe the market and identify no session or order.

struct Trade {
    Price price;
    Quantity quantity;
    Side aggressor_side;

    friend bool operator==(const Trade&, const Trade&) = default;
};

struct BookUpdate {
    Side side;
    Price price;
    Quantity total_quantity; // New total of the level; 0 means the level no longer exists.

    friend bool operator==(const BookUpdate&, const BookUpdate&) = default;
};

using Event = std::variant<OrderAccepted, OrderRejected, Fill, OrderCancelled, CancelRejected,
                           SnapshotBegin, SnapshotLevel, SnapshotEnd, Trade, BookUpdate>;

} // namespace exchange
