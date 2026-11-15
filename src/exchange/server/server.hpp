#pragma once

#include "exchange/core/command.hpp"
#include "exchange/core/event.hpp"
#include "exchange/core/matching_engine.hpp"
#include "exchange/core/types.hpp"
#include "exchange/net/file_descriptor.hpp"
#include "exchange/protocol/messages.hpp"
#include "exchange/server/engine_pipeline.hpp"
#include "exchange/server/publisher.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace exchange::server {

enum class ServerMode : std::uint8_t {
    // One thread: the network thread calls the matching engine directly.
    Inline,
    // Two threads: a network thread and an engine thread, connected by queues.
    Pipeline,
};

struct ServerConfig {
    std::string address = "127.0.0.1";
    // 0 lets the operating system choose a free port; see Server::port().
    std::uint16_t port = 9000;
    // A session whose unsent output exceeds this is closed (SPEC section 10.1).
    std::size_t max_output_bytes = std::size_t{1} << 20;
    // Size requested for the kernel's send buffer of each connection; 0 keeps
    // the system default. A small buffer makes a slow client show up sooner.
    int socket_send_buffer_bytes = 0;
    // When stopping, how long to keep trying to send answers that clients have
    // not read yet (SPEC section 9.4).
    std::chrono::milliseconds shutdown_grace{1000};
    ServerMode mode = ServerMode::Pipeline;
    // Used in pipeline mode only.
    PipelineConfig pipeline{};
};

// The exchange server (SPEC section 9). A network thread runs the event loop:
// it decodes client messages, turns them into commands for the matching engine
// and writes the resulting messages back. Depending on the mode, the engine
// runs on that same thread or on a thread of its own.
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
        // Has decoded input that could not be handed to the engine yet,
        // because the command queue was full.
        bool stalled = false;
        std::vector<std::byte> input;
        std::vector<std::byte> output;
    };

    // Handles one socket that epoll reported as ready.
    void on_ready(SessionId session_id, std::uint32_t ready_events);
    void accept_connections();
    void on_readable(Connection& connection);
    // Decodes and handles the messages buffered for a connection. Stops early,
    // keeping the rest buffered, if the engine cannot take more commands.
    void process_input(Connection& connection);
    // Returns false if the message could not be handled yet and must be retried.
    [[nodiscard]] bool handle_message(Connection& connection,
                                      const protocol::ClientMessage& message);

    // Hands a command to the engine. Returns false, in pipeline mode only, if
    // the command queue is full: the caller must keep the command and retry.
    [[nodiscard]] bool submit(const Command& command);
    // For commands the server creates itself and must not lose: queued here
    // if the engine cannot take them now.
    void submit_or_defer(const Command& command);
    void publish(const Event& event);
    // Pipeline mode: publishes the events the engine thread has produced.
    void drain_engine_events();
    // Pipeline mode: retries what a full command queue held back.
    void retry_held_commands();
    [[nodiscard]] bool commands_held_back() const noexcept {
        return !deferred_commands_.empty() || !stalled_.empty();
    }
    // Queues a message for a session. Returns false if the session is gone or closing.
    bool send(SessionId session_id, const protocol::ServerMessage& message);

    void flush(Connection& connection);
    void watch_writable(Connection& connection, bool enable);
    void schedule_close(Connection& connection,
                        std::optional<protocol::ProtocolErrorCode> error = std::nullopt);
    // Sends pending output and closes scheduled connections, until neither is left.
    void finish_iteration();
    void close_scheduled_connections();
    // Orderly stop: finishes the work already accepted, sends the answers,
    // then closes every connection.
    void shut_down();
    void send_remaining_output();

    ServerConfig config_;
    net::FileDescriptor listener_;
    net::FileDescriptor epoll_;
    net::FileDescriptor stop_event_;
    net::FileDescriptor engine_event_;
    std::uint16_t port_ = 0;

    // Keyed by SessionId, never by file descriptor: the kernel reuses
    // descriptor numbers as soon as a socket is closed, session identifiers
    // are never reused.
    std::unordered_map<SessionId, Connection> connections_;
    SessionId next_session_id_ = 1;
    std::vector<SessionId> dirty_;
    std::vector<SessionId> closing_;

    std::vector<SessionId> stalled_;
    std::deque<Command> deferred_commands_;
    // Pipeline mode: commands handed to the engine thread so far.
    std::uint64_t commands_submitted_ = 0;
    bool shutting_down_ = false;

    // Inline mode uses engine_; pipeline mode uses pipeline_, which owns its
    // own engine on its own thread.
    MatchingEngine engine_;
    std::unique_ptr<EnginePipeline> pipeline_;
    Publisher publisher_;
    std::vector<Event> events_;
    std::array<std::byte, std::size_t{64} * 1024> read_buffer_{};
};

} // namespace exchange::server
