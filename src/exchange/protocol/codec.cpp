#include "exchange/protocol/codec.hpp"

#include <cassert>
#include <optional>

namespace exchange::protocol {

namespace {

// Message types and body lengths, from the tables of SPEC sections 7.2 and 7.3.
enum class MessageType : std::uint8_t {
    Hello = 0x01,
    NewOrder = 0x02,
    CancelOrder = 0x03,
    Subscribe = 0x04,
    HelloAck = 0x81,
    OrderAccepted = 0x82,
    OrderRejected = 0x83,
    Fill = 0x84,
    OrderCancelled = 0x85,
    CancelRejected = 0x86,
    Trade = 0x87,
    BookUpdate = 0x88,
    SnapshotBegin = 0x89,
    SnapshotLevel = 0x8A,
    SnapshotEnd = 0x8B,
    ProtocolError = 0x8F,
};

std::optional<std::size_t> client_body_size(std::uint8_t type) {
    switch (static_cast<MessageType>(type)) {
    case MessageType::Hello:
        return 6;
    case MessageType::NewOrder:
        return 25;
    case MessageType::CancelOrder:
        return 8;
    case MessageType::Subscribe:
        return 0;
    default:
        return std::nullopt;
    }
}

std::optional<std::size_t> server_body_size(std::uint8_t type) {
    switch (static_cast<MessageType>(type)) {
    case MessageType::HelloAck:
        return 10;
    case MessageType::OrderAccepted:
        return 16;
    case MessageType::OrderRejected:
    case MessageType::CancelRejected:
        return 9;
    case MessageType::Fill:
        return 32;
    case MessageType::OrderCancelled:
    case MessageType::SnapshotLevel:
        return 17;
    case MessageType::Trade:
    case MessageType::BookUpdate:
        return 25;
    case MessageType::SnapshotBegin:
        return 8;
    case MessageType::SnapshotEnd:
        return 0;
    case MessageType::ProtocolError:
        return 1;
    default:
        return std::nullopt;
    }
}

// --- Enumerated fields (section 7.4) ---
//
// Wire values are written out explicitly instead of casting the C++
// enumerations, so that reordering an enumeration in the code can never change
// the protocol.

std::uint8_t to_wire(Side side) {
    return side == Side::Buy ? 0 : 1;
}

bool from_wire(std::uint8_t value, Side& side) {
    if (value > 1) {
        return false;
    }
    side = value == 0 ? Side::Buy : Side::Sell;
    return true;
}

std::uint8_t to_wire(RejectReason reason) {
    switch (reason) {
    case RejectReason::InvalidClientOrderId:
        return 1;
    case RejectReason::InvalidPrice:
        return 2;
    case RejectReason::InvalidQuantity:
        return 3;
    }
    return 0;
}

bool from_wire(std::uint8_t value, RejectReason& reason) {
    switch (value) {
    case 1:
        reason = RejectReason::InvalidClientOrderId;
        return true;
    case 2:
        reason = RejectReason::InvalidPrice;
        return true;
    case 3:
        reason = RejectReason::InvalidQuantity;
        return true;
    default:
        return false;
    }
}

std::uint8_t to_wire(CancelRejectReason reason) {
    switch (reason) {
    case CancelRejectReason::UnknownOrder:
        return 1;
    }
    return 0;
}

bool from_wire(std::uint8_t value, CancelRejectReason& reason) {
    if (value != 1) {
        return false;
    }
    reason = CancelRejectReason::UnknownOrder;
    return true;
}

std::uint8_t to_wire(CancelReason reason) {
    switch (reason) {
    case CancelReason::Requested:
        return 1;
    case CancelReason::SessionClosed:
        return 2;
    }
    return 0;
}

bool from_wire(std::uint8_t value, CancelReason& reason) {
    switch (value) {
    case 1:
        reason = CancelReason::Requested;
        return true;
    case 2:
        reason = CancelReason::SessionClosed;
        return true;
    default:
        return false;
    }
}

std::uint8_t to_wire(ProtocolErrorCode code) {
    return static_cast<std::uint8_t>(code);
}

bool from_wire(std::uint8_t value, ProtocolErrorCode& code) {
    if (value < 1 || value > 6) {
        return false;
    }
    code = static_cast<ProtocolErrorCode>(value);
    return true;
}

// --- Writing ---

// Appends integers to a byte vector in little-endian order: the least
// significant byte first. Shifting and masking gives the same bytes on every
// processor, whatever its native byte order.
class Writer {
public:
    Writer(std::vector<std::byte>& out, MessageType type, std::size_t body_size)
        : out_(out), expected_size_(out.size() + header_size + body_size) {
        u16(static_cast<std::uint16_t>(body_size));
        u8(static_cast<std::uint8_t>(type));
    }

    // Checks that the fields written match the announced body size.
    ~Writer() { assert(out_.size() == expected_size_); }

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void u8(std::uint8_t value) { out_.push_back(static_cast<std::byte>(value)); }

    void u16(std::uint16_t value) { integer(value, 2); }
    void u32(std::uint32_t value) { integer(value, 4); }
    void u64(std::uint64_t value) { integer(value, 8); }
    // Two's complement: the same 64 bits, reinterpreted as unsigned.
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

    template <typename Enum> void enumerated(Enum value) { u8(to_wire(value)); }

private:
    void integer(std::uint64_t value, unsigned byte_count) {
        for (unsigned i = 0; i < byte_count; ++i) {
            out_.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFU));
        }
    }

    std::vector<std::byte>& out_;
    // Only read by the assertion, which release builds compile out.
    [[maybe_unused]] std::size_t expected_size_;
};

struct ClientEncoder {
    std::vector<std::byte>& out;

    void operator()(const Hello& m) const {
        Writer writer(out, MessageType::Hello, 6);
        writer.u32(m.magic);
        writer.u16(m.version);
    }
    void operator()(const NewOrder& m) const {
        Writer writer(out, MessageType::NewOrder, 25);
        writer.u64(m.client_order_id);
        writer.enumerated(m.side);
        writer.i64(m.price);
        writer.u64(m.quantity);
    }
    void operator()(const CancelOrder& m) const {
        Writer writer(out, MessageType::CancelOrder, 8);
        writer.u64(m.order_id);
    }
    void operator()(const Subscribe& /*m*/) const {
        const Writer writer(out, MessageType::Subscribe, 0);
    }
};

struct ServerEncoder {
    std::vector<std::byte>& out;

    void operator()(const HelloAck& m) const {
        Writer writer(out, MessageType::HelloAck, 10);
        writer.u16(m.version);
        writer.u64(m.session_id);
    }
    void operator()(const OrderAccepted& m) const {
        Writer writer(out, MessageType::OrderAccepted, 16);
        writer.u64(m.client_order_id);
        writer.u64(m.order_id);
    }
    void operator()(const OrderRejected& m) const {
        Writer writer(out, MessageType::OrderRejected, 9);
        writer.u64(m.client_order_id);
        writer.enumerated(m.reason);
    }
    void operator()(const Fill& m) const {
        Writer writer(out, MessageType::Fill, 32);
        writer.u64(m.order_id);
        writer.i64(m.price);
        writer.u64(m.quantity);
        writer.u64(m.remaining_quantity);
    }
    void operator()(const OrderCancelled& m) const {
        Writer writer(out, MessageType::OrderCancelled, 17);
        writer.u64(m.order_id);
        writer.u64(m.cancelled_quantity);
        writer.enumerated(m.reason);
    }
    void operator()(const CancelRejected& m) const {
        Writer writer(out, MessageType::CancelRejected, 9);
        writer.u64(m.order_id);
        writer.enumerated(m.reason);
    }
    void operator()(const Trade& m) const {
        Writer writer(out, MessageType::Trade, 25);
        writer.u64(m.sequence);
        writer.i64(m.price);
        writer.u64(m.quantity);
        writer.enumerated(m.aggressor_side);
    }
    void operator()(const BookUpdate& m) const {
        Writer writer(out, MessageType::BookUpdate, 25);
        writer.u64(m.sequence);
        writer.enumerated(m.side);
        writer.i64(m.price);
        writer.u64(m.total_quantity);
    }
    void operator()(const SnapshotBegin& m) const {
        Writer writer(out, MessageType::SnapshotBegin, 8);
        writer.u64(m.last_sequence);
    }
    void operator()(const SnapshotLevel& m) const {
        Writer writer(out, MessageType::SnapshotLevel, 17);
        writer.enumerated(m.side);
        writer.i64(m.price);
        writer.u64(m.total_quantity);
    }
    void operator()(const SnapshotEnd& /*m*/) const {
        const Writer writer(out, MessageType::SnapshotEnd, 0);
    }
    void operator()(const ProtocolError& m) const {
        Writer writer(out, MessageType::ProtocolError, 1);
        writer.enumerated(m.code);
    }
};

// --- Reading ---

// Reads little-endian integers from the body of a message. The caller has
// already checked that the body has the length its type requires, so reads
// never go past the end.
class Reader {
public:
    explicit Reader(std::span<const std::byte> body) : body_(body) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(integer(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(integer(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(integer(4)); }
    std::uint64_t u64() { return integer(8); }
    std::int64_t i64() { return static_cast<std::int64_t>(integer(8)); }

    // Reads a one-byte enumerated field. Returns false if the value is undefined.
    template <typename Enum> bool enumerated(Enum& value) { return from_wire(u8(), value); }

private:
    std::uint64_t integer(std::size_t byte_count) {
        assert(position_ + byte_count <= body_.size());
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < byte_count; ++i) {
            value |= std::to_integer<std::uint64_t>(body_[position_ + i]) << (8 * i);
        }
        position_ += byte_count;
        return value;
    }

    std::span<const std::byte> body_;
    std::size_t position_ = 0;
};

// The outcome of reading a header and locating the body.
struct Frame {
    DecodeStatus status = DecodeStatus::Incomplete;
    ProtocolErrorCode error = ProtocolErrorCode::BadLength;
    MessageType type{};
    std::span<const std::byte> body;
};

// `body_size_of` gives the body length of a type this side accepts, or nullopt.
template <typename BodySizeOf>
Frame read_frame(std::span<const std::byte> bytes, BodySizeOf body_size_of) {
    Frame frame;
    if (bytes.size() < header_size) {
        return frame;
    }
    Reader header(bytes.first(header_size));
    const std::size_t length = header.u16();
    const std::uint8_t type = header.u8();

    frame.status = DecodeStatus::Invalid;
    if (length > max_body_size) {
        frame.error = ProtocolErrorCode::BadLength;
        return frame;
    }
    const std::optional<std::size_t> expected_length = body_size_of(type);
    if (!expected_length.has_value()) {
        frame.error = ProtocolErrorCode::UnknownType;
        return frame;
    }
    if (length != *expected_length) {
        frame.error = ProtocolErrorCode::BadLength;
        return frame;
    }

    if (bytes.size() < header_size + length) {
        frame.status = DecodeStatus::Incomplete;
        return frame;
    }
    frame.status = DecodeStatus::Complete;
    frame.type = static_cast<MessageType>(type);
    frame.body = bytes.subspan(header_size, length);
    return frame;
}

// Fills `message` from a frame's body. Returns false if an enumerated field is invalid.
bool parse(const Frame& frame, ClientMessage& message) {
    Reader reader(frame.body);
    switch (frame.type) {
    case MessageType::Hello: {
        Hello hello{};
        hello.magic = reader.u32();
        hello.version = reader.u16();
        message = hello;
        return true;
    }
    case MessageType::NewOrder: {
        NewOrder order{};
        order.client_order_id = reader.u64();
        const bool side_ok = reader.enumerated(order.side);
        order.price = reader.i64();
        order.quantity = reader.u64();
        message = order;
        return side_ok;
    }
    case MessageType::CancelOrder:
        message = CancelOrder{.order_id = reader.u64()};
        return true;
    case MessageType::Subscribe:
        message = Subscribe{};
        return true;
    default:
        return false;
    }
}

bool parse_order_event(const Frame& frame, ServerMessage& message) {
    Reader reader(frame.body);
    switch (frame.type) {
    case MessageType::HelloAck: {
        HelloAck ack{};
        ack.version = reader.u16();
        ack.session_id = reader.u64();
        message = ack;
        return true;
    }
    case MessageType::OrderAccepted: {
        OrderAccepted accepted{};
        accepted.client_order_id = reader.u64();
        accepted.order_id = reader.u64();
        message = accepted;
        return true;
    }
    case MessageType::OrderRejected: {
        OrderRejected rejected{};
        rejected.client_order_id = reader.u64();
        const bool reason_ok = reader.enumerated(rejected.reason);
        message = rejected;
        return reason_ok;
    }
    case MessageType::Fill: {
        Fill fill{};
        fill.order_id = reader.u64();
        fill.price = reader.i64();
        fill.quantity = reader.u64();
        fill.remaining_quantity = reader.u64();
        message = fill;
        return true;
    }
    case MessageType::OrderCancelled: {
        OrderCancelled cancelled{};
        cancelled.order_id = reader.u64();
        cancelled.cancelled_quantity = reader.u64();
        const bool reason_ok = reader.enumerated(cancelled.reason);
        message = cancelled;
        return reason_ok;
    }
    case MessageType::CancelRejected: {
        CancelRejected rejected{};
        rejected.order_id = reader.u64();
        const bool reason_ok = reader.enumerated(rejected.reason);
        message = rejected;
        return reason_ok;
    }
    case MessageType::ProtocolError: {
        ProtocolError error{};
        const bool code_ok = reader.enumerated(error.code);
        message = error;
        return code_ok;
    }
    default:
        return false;
    }
}

bool parse_market_data(const Frame& frame, ServerMessage& message) {
    Reader reader(frame.body);
    switch (frame.type) {
    case MessageType::Trade: {
        Trade trade{};
        trade.sequence = reader.u64();
        trade.price = reader.i64();
        trade.quantity = reader.u64();
        const bool side_ok = reader.enumerated(trade.aggressor_side);
        message = trade;
        return side_ok;
    }
    case MessageType::BookUpdate: {
        BookUpdate update{};
        update.sequence = reader.u64();
        const bool side_ok = reader.enumerated(update.side);
        update.price = reader.i64();
        update.total_quantity = reader.u64();
        message = update;
        return side_ok;
    }
    case MessageType::SnapshotBegin:
        message = SnapshotBegin{.last_sequence = reader.u64()};
        return true;
    case MessageType::SnapshotLevel: {
        SnapshotLevel level{};
        const bool side_ok = reader.enumerated(level.side);
        level.price = reader.i64();
        level.total_quantity = reader.u64();
        message = level;
        return side_ok;
    }
    case MessageType::SnapshotEnd:
        message = SnapshotEnd{};
        return true;
    default:
        return false;
    }
}

bool parse(const Frame& frame, ServerMessage& message) {
    switch (frame.type) {
    case MessageType::Trade:
    case MessageType::BookUpdate:
    case MessageType::SnapshotBegin:
    case MessageType::SnapshotLevel:
    case MessageType::SnapshotEnd:
        return parse_market_data(frame, message);
    default:
        return parse_order_event(frame, message);
    }
}

template <typename Message, typename BodySizeOf>
Decoded<Message> decode(std::span<const std::byte> bytes, BodySizeOf body_size_of) {
    Decoded<Message> result;
    const Frame frame = read_frame(bytes, body_size_of);
    result.status = frame.status;
    result.error = frame.error;
    if (frame.status != DecodeStatus::Complete) {
        return result;
    }
    if (!parse(frame, result.message)) {
        result.status = DecodeStatus::Invalid;
        result.error = ProtocolErrorCode::BadField;
        result.message = Message{};
        return result;
    }
    result.size = header_size + frame.body.size();
    return result;
}

} // namespace

void encode(const ClientMessage& message, std::vector<std::byte>& out) {
    std::visit(ClientEncoder{out}, message);
}

void encode(const ServerMessage& message, std::vector<std::byte>& out) {
    std::visit(ServerEncoder{out}, message);
}

Decoded<ClientMessage> decode_client_message(std::span<const std::byte> bytes) {
    return decode<ClientMessage>(bytes, client_body_size);
}

Decoded<ServerMessage> decode_server_message(std::span<const std::byte> bytes) {
    return decode<ServerMessage>(bytes, server_body_size);
}

} // namespace exchange::protocol
