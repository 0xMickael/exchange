#pragma once

#include "exchange/core/command.hpp"
#include "exchange/core/event.hpp"
#include "exchange/core/matching_engine.hpp"
#include "exchange/core/types.hpp"
#include "exchange/net/file_descriptor.hpp"
#include "exchange/protocol/messages.hpp"
#include "exchange/server/publisher.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace exchange::server {

struct ServerConfig {
    std::string address = "127.0.0.1";
    // 0 lets the operating system choose a free port; see Server::port().
    std::uint16_t port = 9000;
    // A session whose unsent output exceeds this is closed (SPEC section 10.1).
    std::size_t max_output_bytes = std::size_t{1} << 20;
};

// The exchange server in inline mode (SPEC section 9.2): one thread runs the
// event loop, decodes client messages, calls the matching engine directly and
// writes the resulting messages back.
//
// The event loop asks the kernel which sockets are ready (epoll), handles each
// of them without ever blocking, and starts again. No socket operation waits:
// a read returns what has already arrived, a write sends what the kernel can
// take now and the rest is kept in a buffer.
class Server {
public:
    // Opens the listening socket. Throws std::system_error if that fails, for
    // example because the port is already in use.
    explicit Server(const ServerConfig& config);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // The port the server listens on.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Runs the event loop on the calling thread until request_stop() is called.
    void run();

    // Makes run() return. May be called from any thread and from a signal handler.
    void request_stop() noexcept;

private:
    struct Connection {
        net::FileDescriptor socket;
        SessionId session_id = 0;
        bool hello_received = false;
        // Scheduled to be closed at the end of the current loop iteration.
        bool closing = false;
        std::optional<protocol::ProtocolErrorCode> close_error;
        // Has unsent output queued during the current loop iteration.
        bool dirty = false;
        // The kernel refused part of the output: waiting for the socket to
        // become writable again.
        bool waiting_for_writable = false;
        std::vector<std::byte> input;
        std::vector<std::byte> output;
    };

    // Handles one socket that epoll reported as ready.
    void on_ready(SessionId session_id, std::uint32_t ready_events);
    void accept_connections();
    void on_readable(Connection& connection);
    void handle_message(Connection& connection, const protocol::ClientMessage& message);

    // Runs a command through the engine and publishes the resulting events.
    void submit(const Command& command);
    // Queues a message for a session. Returns false if the session is gone or closing.
    bool send(SessionId session_id, const protocol::ServerMessage& message);

    void flush(Connection& connection);
    void watch_writable(Connection& connection, bool enable);
    void schedule_close(Connection& connection,
                        std::optional<protocol::ProtocolErrorCode> error = std::nullopt);
    // Sends pending output and closes scheduled connections, until neither is left.
    void finish_iteration();
    void close_scheduled_connections();

    ServerConfig config_;
    net::FileDescriptor listener_;
    net::FileDescriptor epoll_;
    net::FileDescriptor stop_event_;
    std::uint16_t port_ = 0;

    // Keyed by SessionId, never by file descriptor: the kernel reuses
    // descriptor numbers as soon as a socket is closed, session identifiers
    // are never reused.
    std::unordered_map<SessionId, Connection> connections_;
    SessionId next_session_id_ = 1;
    std::vector<SessionId> dirty_;
    std::vector<SessionId> closing_;

    MatchingEngine engine_;
    Publisher publisher_;
    std::vector<Event> events_;
    std::array<std::byte, std::size_t{64} * 1024> read_buffer_{};
};

} // namespace exchange::server
