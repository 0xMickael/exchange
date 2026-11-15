#include "exchange/protocol/codec.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <span>
#include <vector>

namespace exchange::protocol {
namespace {

using Bytes = std::vector<std::byte>;

Bytes bytes_of(std::initializer_list<unsigned> values) {
    Bytes bytes;
    for (const unsigned value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

template <typename Message> Bytes encoded(const Message& message) {
    Bytes bytes;
    encode(message, bytes);
    return bytes;
}

constexpr std::uint64_t max_u64 = std::numeric_limits<std::uint64_t>::max();
constexpr Price min_i64 = std::numeric_limits<Price>::min();

// One of each message, with values that exercise every byte of every field.
std::vector<ClientMessage> all_client_messages() {
    return {
        Hello{.magic = magic, .version = version},
        NewOrder{.client_order_id = 1, .side = Side::Buy, .price = 101, .quantity = 5},
        NewOrder{.client_order_id = max_u64,
                 .side = Side::Sell,
                 .price = min_i64,
                 .quantity = 0x0102030405060708ULL},
        CancelOrder{.order_id = 0x1122334455667788ULL},
        Subscribe{},
    };
}

std::vector<ServerMessage> all_server_messages() {
    return {
        HelloAck{.version = version, .session_id = 42},
        OrderAccepted{.client_order_id = 7, .order_id = 9},
        OrderRejected{.client_order_id = 7, .reason = RejectReason::InvalidClientOrderId},
        OrderRejected{.client_order_id = 8, .reason = RejectReason::InvalidPrice},
        OrderRejected{.client_order_id = 9, .reason = RejectReason::InvalidQuantity},
        Fill{.order_id = 9, .price = 101, .quantity = 5, .remaining_quantity = max_u64},
        OrderCancelled{.order_id = 9, .cancelled_quantity = 3, .reason = CancelReason::Requested},
        OrderCancelled{
            .order_id = 9, .cancelled_quantity = 3, .reason = CancelReason::SessionClosed},
        CancelRejected{.order_id = 9, .reason = CancelRejectReason::UnknownOrder},
        Trade{.sequence = 1, .price = -1, .quantity = 5, .aggressor_side = Side::Sell},
        BookUpdate{.sequence = 2, .side = Side::Buy, .price = 100, .total_quantity = 0},
        SnapshotBegin{.last_sequence = 2},
        SnapshotLevel{.side = Side::Sell, .price = 1'000'000'000, .total_quantity = 12},
        SnapshotEnd{},
        ProtocolError{.code = ProtocolErrorCode::BadLength},
        ProtocolError{.code = ProtocolErrorCode::UnsupportedVersion},
    };
}

// --- Exact bytes: these tests pin the wire format of SPEC section 7 ---

TEST(Codec, HelloBytes) {
    EXPECT_EQ(encoded(ClientMessage{Hello{.magic = magic, .version = version}}),
              bytes_of({0x06, 0x00, 0x01, 'E', 'X', 'C', 'H', 0x01, 0x00}));
}

TEST(Codec, NewOrderBytes) {
    const ClientMessage message =
        NewOrder{.client_order_id = 1, .side = Side::Sell, .price = 101, .quantity = 5};

    EXPECT_EQ(encoded(message), bytes_of({
                                    0x19, 0x00,                                     // length 25
                                    0x02,                                           // type
                                    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // client id
                                    0x01,                                           // sell
                                    0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // price 101
                                    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // quantity
                                }));
}

TEST(Codec, IntegersAreLittleEndian) {
    EXPECT_EQ(encoded(ClientMessage{CancelOrder{.order_id = 0x1122334455667788ULL}}),
              bytes_of({0x08, 0x00, 0x03, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}));
}

TEST(Codec, NegativePriceIsTwosComplement) {
    const Bytes bytes =
        encoded(ServerMessage{SnapshotLevel{.side = Side::Buy, .price = -2, .total_quantity = 0}});

    ASSERT_EQ(bytes.size(), 20U);
    EXPECT_EQ(std::span(bytes).subspan(4, 8).front(), std::byte{0xFE});
    for (const std::byte byte : std::span(bytes).subspan(5, 7)) {
        EXPECT_EQ(byte, std::byte{0xFF});
    }
}

TEST(Codec, EmptyBodyMessages) {
    EXPECT_EQ(encoded(ClientMessage{Subscribe{}}), bytes_of({0x00, 0x00, 0x04}));
    EXPECT_EQ(encoded(ServerMessage{SnapshotEnd{}}), bytes_of({0x00, 0x00, 0x8B}));
}

TEST(Codec, FillBytes) {
    const ServerMessage message =
        Fill{.order_id = 2, .price = 3, .quantity = 4, .remaining_quantity = 5};
    const Bytes bytes = encoded(message);

    ASSERT_EQ(bytes.size(), 35U);
    EXPECT_EQ(bytes[0], std::byte{32});
    EXPECT_EQ(bytes[1], std::byte{0});
    EXPECT_EQ(bytes[2], std::byte{0x84});
    EXPECT_EQ(bytes[3], std::byte{2});
    EXPECT_EQ(bytes[11], std::byte{3});
    EXPECT_EQ(bytes[19], std::byte{4});
    EXPECT_EQ(bytes[27], std::byte{5});
}

// --- Round trips ---

TEST(Codec, ClientMessagesSurviveARoundTrip) {
    for (const ClientMessage& message : all_client_messages()) {
        const Bytes bytes = encoded(message);
        const Decoded<ClientMessage> decoded = decode_client_message(bytes);

        ASSERT_EQ(decoded.status, DecodeStatus::Complete) << "message " << message.index();
        EXPECT_EQ(decoded.size, bytes.size());
        EXPECT_EQ(decoded.message, message);
    }
}

TEST(Codec, ServerMessagesSurviveARoundTrip) {
    for (const ServerMessage& message : all_server_messages()) {
        const Bytes bytes = encoded(message);
        const Decoded<ServerMessage> decoded = decode_server_message(bytes);

        ASSERT_EQ(decoded.status, DecodeStatus::Complete) << "message " << message.index();
        EXPECT_EQ(decoded.size, bytes.size());
        EXPECT_EQ(decoded.message, message);
    }
}

TEST(Codec, EncodeAppendsToTheBuffer) {
    Bytes bytes = bytes_of({0xAA});
    encode(ClientMessage{Subscribe{}}, bytes);

    EXPECT_EQ(bytes, bytes_of({0xAA, 0x00, 0x00, 0x04}));
}

// --- Framing: TCP can cut the stream anywhere ---

TEST(Codec, EveryStrictPrefixOfAMessageIsIncomplete) {
    for (const ServerMessage& message : all_server_messages()) {
        const Bytes bytes = encoded(message);
        for (std::size_t length = 0; length < bytes.size(); ++length) {
            const Decoded<ServerMessage> decoded =
                decode_server_message(std::span(bytes).first(length));
            ASSERT_EQ(decoded.status, DecodeStatus::Incomplete)
                << "message " << message.index() << ", " << length << " bytes";
        }
    }
}

TEST(Codec, DecodesOnlyTheFirstMessageOfABuffer) {
    Bytes bytes = encoded(ClientMessage{CancelOrder{.order_id = 5}});
    const std::size_t first_size = bytes.size();
    encode(ClientMessage{Subscribe{}}, bytes);

    const Decoded<ClientMessage> first = decode_client_message(bytes);
    ASSERT_EQ(first.status, DecodeStatus::Complete);
    EXPECT_EQ(first.size, first_size);
    EXPECT_EQ(first.message, ClientMessage{CancelOrder{.order_id = 5}});

    const Decoded<ClientMessage> second =
        decode_client_message(std::span(bytes).subspan(first.size));
    ASSERT_EQ(second.status, DecodeStatus::Complete);
    EXPECT_EQ(second.message, ClientMessage{Subscribe{}});
}

// Delivers a byte stream in chunks of a fixed size, the way successive reads
// from a socket would, and decodes whatever is complete after each chunk.
std::vector<ServerMessage> decode_in_chunks(const Bytes& stream, std::size_t chunk_size) {
    std::vector<ServerMessage> messages;
    Bytes pending;
    for (std::size_t offset = 0; offset < stream.size(); offset += chunk_size) {
        const std::size_t count = std::min(chunk_size, stream.size() - offset);
        const auto chunk = std::span(stream).subspan(offset, count);
        pending.insert(pending.end(), chunk.begin(), chunk.end());

        std::size_t consumed = 0;
        while (true) {
            const Decoded<ServerMessage> decoded =
                decode_server_message(std::span(pending).subspan(consumed));
            if (decoded.status != DecodeStatus::Complete) {
                EXPECT_EQ(decoded.status, DecodeStatus::Incomplete);
                break;
            }
            messages.push_back(decoded.message);
            consumed += decoded.size;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    EXPECT_TRUE(pending.empty());
    return messages;
}

TEST(Codec, AStreamCutAtAnyChunkSizeDecodesToTheSameMessages) {
    const std::vector<ServerMessage> messages = all_server_messages();
    Bytes stream;
    for (const ServerMessage& message : messages) {
        encode(message, stream);
    }

    for (std::size_t chunk_size = 1; chunk_size <= stream.size(); ++chunk_size) {
        ASSERT_EQ(decode_in_chunks(stream, chunk_size), messages) << "chunk size " << chunk_size;
    }
}

// --- Protocol errors (section 7.6) ---

TEST(Codec, UnknownTypeIsInvalid) {
    const Decoded<ClientMessage> decoded = decode_client_message(bytes_of({0x00, 0x00, 0x7F}));

    EXPECT_EQ(decoded.status, DecodeStatus::Invalid);
    EXPECT_EQ(decoded.error, ProtocolErrorCode::UnknownType);
}

TEST(Codec, EachSideOnlyAcceptsTheMessagesAddressedToIt) {
    const Bytes client_bytes = encoded(ClientMessage{Subscribe{}});
    const Bytes server_bytes = encoded(ServerMessage{SnapshotEnd{}});

    EXPECT_EQ(decode_server_message(client_bytes).error, ProtocolErrorCode::UnknownType);
    EXPECT_EQ(decode_server_message(client_bytes).status, DecodeStatus::Invalid);
    EXPECT_EQ(decode_client_message(server_bytes).error, ProtocolErrorCode::UnknownType);
    EXPECT_EQ(decode_client_message(server_bytes).status, DecodeStatus::Invalid);
}

TEST(Codec, LengthThatDoesNotMatchTheTypeIsInvalid) {
    // CancelOrder announces 9 bytes instead of 8.
    const Decoded<ClientMessage> decoded = decode_client_message(bytes_of({0x09, 0x00, 0x03}));

    EXPECT_EQ(decoded.status, DecodeStatus::Invalid);
    EXPECT_EQ(decoded.error, ProtocolErrorCode::BadLength);
}

TEST(Codec, OversizedLengthIsInvalidWithoutWaitingForTheBody) {
    // 65535 bytes announced: rejected from the header alone.
    const Decoded<ClientMessage> decoded = decode_client_message(bytes_of({0xFF, 0xFF, 0x02}));

    EXPECT_EQ(decoded.status, DecodeStatus::Invalid);
    EXPECT_EQ(decoded.error, ProtocolErrorCode::BadLength);
}

TEST(Codec, UndefinedSideIsInvalid) {
    Bytes bytes = encoded(ClientMessage{
        NewOrder{.client_order_id = 1, .side = Side::Buy, .price = 1, .quantity = 1}});
    bytes[header_size + 8] = std::byte{2};

    const Decoded<ClientMessage> decoded = decode_client_message(bytes);

    EXPECT_EQ(decoded.status, DecodeStatus::Invalid);
    EXPECT_EQ(decoded.error, ProtocolErrorCode::BadField);
}

TEST(Codec, UndefinedReasonIsInvalid) {
    for (const unsigned reason : {0U, 4U, 255U}) {
        Bytes bytes = encoded(ServerMessage{
            OrderRejected{.client_order_id = 1, .reason = RejectReason::InvalidPrice}});
        bytes.back() = static_cast<std::byte>(reason);

        const Decoded<ServerMessage> decoded = decode_server_message(bytes);

        EXPECT_EQ(decoded.status, DecodeStatus::Invalid) << reason;
        EXPECT_EQ(decoded.error, ProtocolErrorCode::BadField) << reason;
    }
}

TEST(Codec, PriceAndQuantityAreNotRangeChecked) {
    // Out-of-range values are well-formed messages; the engine rejects them.
    const ClientMessage message =
        NewOrder{.client_order_id = 1, .side = Side::Buy, .price = -7, .quantity = max_u64};

    const Decoded<ClientMessage> decoded = decode_client_message(encoded(message));

    ASSERT_EQ(decoded.status, DecodeStatus::Complete);
    EXPECT_EQ(decoded.message, message);
}

// --- Arbitrary input ---

// Whatever bytes arrive, the decoder must stay inside the buffer (checked by
// AddressSanitizer) and give a coherent answer. Any buffer it accepts must be
// exactly what encoding the decoded message produces: there is only one way to
// write each message.
TEST(Codec, RandomBytesNeverBreakTheDecoder) {
    std::mt19937_64 rng(2026);
    std::size_t complete = 0;

    for (int i = 0; i < 200'000; ++i) {
        Bytes bytes(rng() % 40);
        for (std::byte& byte : bytes) {
            byte = static_cast<std::byte>(rng() & 0xFFU);
        }
        // Make valid headers common enough to reach the body parsers.
        if (bytes.size() >= header_size && rng() % 2 == 0) {
            const Bytes valid = (rng() % 2 == 0) ? encoded(all_client_messages()[rng() % 5])
                                                 : encoded(all_server_messages()[rng() % 16]);
            std::copy_n(valid.begin(), header_size, bytes.begin());
        }

        const Decoded<ClientMessage> client = decode_client_message(bytes);
        if (client.status == DecodeStatus::Complete) {
            ASSERT_LE(client.size, bytes.size());
            ASSERT_EQ(
                encoded(client.message),
                Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(client.size)));
            ++complete;
        }
        const Decoded<ServerMessage> server = decode_server_message(bytes);
        if (server.status == DecodeStatus::Complete) {
            ASSERT_LE(server.size, bytes.size());
            ASSERT_EQ(
                encoded(server.message),
                Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(server.size)));
            ++complete;
        }
    }
    EXPECT_GT(complete, 1000U) << "the test must exercise the body parsers";
}

} // namespace
} // namespace exchange::protocol
