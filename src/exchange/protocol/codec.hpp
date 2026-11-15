#pragma once

#include "exchange/protocol/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Conversion between protocol messages and bytes (SPEC section 7).
//
// These functions do no I/O. A connection keeps the bytes it has received in a
// buffer and calls a decode function on it: the result says whether the buffer
// starts with a whole message, with the beginning of one, or with something
// that is not a message.
namespace exchange::protocol {

// Appends the header and body of one message to `out`.
void encode(const ClientMessage& message, std::vector<std::byte>& out);
void encode(const ServerMessage& message, std::vector<std::byte>& out);

enum class DecodeStatus : std::uint8_t {
    // `bytes` starts with a whole, valid message.
    Complete,
    // `bytes` is the beginning of a message: more bytes must be received.
    Incomplete,
    // `bytes` does not start with a valid message. The stream cannot be used
    // any further, because nothing says where the next message would begin.
    Invalid,
};

template <typename Message> struct Decoded {
    DecodeStatus status = DecodeStatus::Incomplete;
    // When Complete: the number of bytes the message occupies, header included.
    std::size_t size = 0;
    // When Complete: the message.
    Message message{};
    // When Invalid: what is wrong.
    ProtocolErrorCode error = ProtocolErrorCode::BadLength;
};

// Decodes the message at the start of `bytes`. Bytes after that message are
// ignored: call again on the rest to decode the next one.
[[nodiscard]] Decoded<ClientMessage> decode_client_message(std::span<const std::byte> bytes);
[[nodiscard]] Decoded<ServerMessage> decode_server_message(std::span<const std::byte> bytes);

} // namespace exchange::protocol
