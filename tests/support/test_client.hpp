#pragma once

#include "exchange/net/file_descriptor.hpp"
#include "exchange/net/tcp.hpp"
#include "exchange/protocol/codec.hpp"
#include "exchange/protocol/messages.hpp"

#include <sys/socket.h>
#include <sys/time.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

namespace exchange::test {

// A simple blocking client for integration tests: it sends protocol messages
// to a server on this machine and reads the answers one at a time.
class TestClient {
public:
    explicit TestClient(std::uint16_t port) : socket_(net::connect_tcp("127.0.0.1", port)) {
        // A test that waits for a message the server never sends must fail,
        // not hang.
        timeval timeout{};
        timeout.tv_sec = 10;
        ::setsockopt(socket_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    void send(const protocol::ClientMessage& message) {
        std::vector<std::byte> bytes;
        protocol::encode(message, bytes);
        send_bytes(bytes);
    }

    void send_bytes(std::span<const std::byte> bytes) {
        std::size_t sent_total = 0;
        while (sent_total < bytes.size()) {
            const ssize_t sent = ::send(socket_.get(), bytes.data() + sent_total,
                                        bytes.size() - sent_total, MSG_NOSIGNAL);
            if (sent <= 0) {
                throw std::runtime_error("TestClient: send failed");
            }
            sent_total += static_cast<std::size_t>(sent);
        }
    }

    // The next message from the server, or nullopt if the server closed the
    // connection. Throws if nothing arrives in time or the bytes are not a
    // valid message.
    std::optional<protocol::ServerMessage> receive() {
        while (true) {
            const protocol::Decoded<protocol::ServerMessage> decoded =
                protocol::decode_server_message(pending_);
            if (decoded.status == protocol::DecodeStatus::Complete) {
                pending_.erase(pending_.begin(),
                               pending_.begin() + static_cast<std::ptrdiff_t>(decoded.size));
                return decoded.message;
            }
            if (decoded.status == protocol::DecodeStatus::Invalid) {
                throw std::runtime_error("TestClient: the server sent an invalid message");
            }
            std::array<std::byte, 4096> buffer{};
            const ssize_t received = ::recv(socket_.get(), buffer.data(), buffer.size(), 0);
            if (received == 0) {
                return std::nullopt;
            }
            if (received < 0 && errno == ECONNRESET) {
                // The server closed the connection while this client still had
                // data on its way to it. Everything the server sent before has
                // been read at this point, so this is the end of the stream too.
                return std::nullopt;
            }
            if (received < 0) {
                throw std::runtime_error("TestClient: no message received in time");
            }
            const auto chunk = std::span(buffer).first(static_cast<std::size_t>(received));
            pending_.insert(pending_.end(), chunk.begin(), chunk.end());
        }
    }

    // The next message, which must be of type T.
    template <typename T> T expect() {
        const std::optional<protocol::ServerMessage> message = receive();
        if (!message.has_value()) {
            throw std::runtime_error("TestClient: connection closed, a message was expected");
        }
        if (!std::holds_alternative<T>(*message)) {
            throw std::runtime_error("TestClient: unexpected message type, index " +
                                     std::to_string(message->index()));
        }
        return std::get<T>(*message);
    }

    // Performs the handshake and returns the session identifier.
    SessionId hello() {
        send(protocol::Hello{.magic = protocol::magic, .version = protocol::version});
        return expect<protocol::HelloAck>().session_id;
    }

    void close() { socket_.reset(); }

    // Shrinks the kernel's receive buffer, so that a client that stops reading
    // fills it after a few kilobytes instead of megabytes.
    void set_receive_buffer(int bytes) {
        ::setsockopt(socket_.get(), SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    }

    // Reads and discards everything until the server closes the connection.
    // Returns false if the connection is still open after the receive timeout.
    bool read_until_closed() {
        std::array<std::byte, 4096> buffer{};
        while (true) {
            const ssize_t received = ::recv(socket_.get(), buffer.data(), buffer.size(), 0);
            if (received == 0) {
                return true;
            }
            if (received < 0) {
                // A reset also means the server closed the connection.
                return errno == ECONNRESET;
            }
        }
    }

private:
    net::FileDescriptor socket_;
    std::vector<std::byte> pending_;
};

} // namespace exchange::test
