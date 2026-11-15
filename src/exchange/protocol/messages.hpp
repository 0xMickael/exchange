#pragma once

#include "exchange/core/event.hpp"
#include "exchange/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <variant>

// The messages of the wire protocol (SPEC section 7), as C++ values.
//
// These are not the bytes that travel on the network: codec.hpp converts
// between these structs and bytes. Several names also exist in the engine
// (exchange::NewOrder, exchange::Fill, ...). The versions here are what a
// client sees: they carry no session identifier, because the connection
// identifies the session, and market data carries a sequence number.
namespace exchange::protocol {

inline constexpr std::uint32_t magic = 0x48435845; // The bytes 'E' 'X' 'C' 'H' on the wire.
inline constexpr std::uint16_t version = 1;
inline constexpr std::size_t header_size = 3;
inline constexpr std::size_t max_body_size = 64;

// The numeric values are the wire values of section 7.4.
enum class ProtocolErrorCode : std::uint8_t {
    BadLength = 1,
    UnknownType = 2,
    BadField = 3,
    HelloRequired = 4,
    BadMagic = 5,
    UnsupportedVersion = 6,
};

// --- Client to server ---

struct Hello {
    std::uint32_t magic;
    std::uint16_t version;

    friend bool operator==(const Hello&, const Hello&) = default;
};

struct NewOrder {
    ClientOrderId client_order_id;
    Side side;
    Price price;
    Quantity quantity;

    friend bool operator==(const NewOrder&, const NewOrder&) = default;
};

struct CancelOrder {
    OrderId order_id;

    friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

struct Subscribe {
    friend bool operator==(const Subscribe&, const Subscribe&) = default;
};

using ClientMessage = std::variant<Hello, NewOrder, CancelOrder, Subscribe>;

// --- Server to client ---

struct HelloAck {
    std::uint16_t version;
    SessionId session_id;

    friend bool operator==(const HelloAck&, const HelloAck&) = default;
};

struct OrderAccepted {
    ClientOrderId client_order_id;
    OrderId order_id;

    friend bool operator==(const OrderAccepted&, const OrderAccepted&) = default;
};

struct OrderRejected {
    ClientOrderId client_order_id;
    RejectReason reason;

    friend bool operator==(const OrderRejected&, const OrderRejected&) = default;
};

struct Fill {
    OrderId order_id;
    Price price;
    Quantity quantity;
    Quantity remaining_quantity;

    friend bool operator==(const Fill&, const Fill&) = default;
};

struct OrderCancelled {
    OrderId order_id;
    Quantity cancelled_quantity;
    CancelReason reason;

    friend bool operator==(const OrderCancelled&, const OrderCancelled&) = default;
};

struct CancelRejected {
    OrderId order_id;
    CancelRejectReason reason;

    friend bool operator==(const CancelRejected&, const CancelRejected&) = default;
};

struct Trade {
    std::uint64_t sequence;
    Price price;
    Quantity quantity;
    Side aggressor_side;

    friend bool operator==(const Trade&, const Trade&) = default;
};

struct BookUpdate {
    std::uint64_t sequence;
    Side side;
    Price price;
    Quantity total_quantity;

    friend bool operator==(const BookUpdate&, const BookUpdate&) = default;
};

struct SnapshotBegin {
    std::uint64_t last_sequence;

    friend bool operator==(const SnapshotBegin&, const SnapshotBegin&) = default;
};

struct SnapshotLevel {
    Side side;
    Price price;
    Quantity total_quantity;

    friend bool operator==(const SnapshotLevel&, const SnapshotLevel&) = default;
};

struct SnapshotEnd {
    friend bool operator==(const SnapshotEnd&, const SnapshotEnd&) = default;
};

struct ProtocolError {
    ProtocolErrorCode code;

    friend bool operator==(const ProtocolError&, const ProtocolError&) = default;
};

using ServerMessage =
    std::variant<HelloAck, OrderAccepted, OrderRejected, Fill, OrderCancelled, CancelRejected,
                 Trade, BookUpdate, SnapshotBegin, SnapshotLevel, SnapshotEnd, ProtocolError>;

} // namespace exchange::protocol
