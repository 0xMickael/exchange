#include "exchange/server/server.hpp"

#include "exchange/net/tcp.hpp"
#include "exchange/protocol/codec.hpp"
#include "exchange/queue/cpu_relax.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include <cerrno>
#include <span>
#include <system_error>
#include <utility>
#include <variant>

namespace exchange::server {

namespace {

// Values stored in epoll events to tell what became ready. Session
// identifiers start at 1 and never reach the maximum, so both are free.
constexpr std::uint64_t listener_token = 0;
constexpr std::uint64_t stop_token = ~std::uint64_t{0};

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

void epoll_control(const net::FileDescriptor& epoll, int operation, int fd, std::uint32_t events,
                   std::uint64_t token) {
    epoll_event event{};
    event.events = events;
    event.data.u64 = token;
    if (::epoll_ctl(epoll.get(), operation, fd, &event) != 0) {
        throw_errno("epoll_ctl");
    }
}

} // namespace

Server::Server(const ServerConfig& config)
    : config_(config), listener_(net::listen_tcp(config.address, config.port)),
      epoll_(::epoll_create1(EPOLL_CLOEXEC)), stop_event_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      port_(net::local_port(listener_)) {
    if (!epoll_.valid()) {
        throw_errno("epoll_create1");
    }
    if (!stop_event_.valid()) {
        throw_errno("eventfd");
    }
    epoll_control(epoll_, EPOLL_CTL_ADD, listener_.get(), EPOLLIN, listener_token);
    epoll_control(epoll_, EPOLL_CTL_ADD, stop_event_.get(), EPOLLIN, stop_token);
    if (config_.mode == ServerMode::Pipeline) {
        pipeline_ = std::make_unique<EnginePipeline>(config_.pipeline);
    }
}

void Server::request_stop() noexcept {
    // An eventfd is a counter in the kernel that epoll can watch. Adding to it
    // wakes the event loop. write() is one of the few functions that are safe
    // to call from a signal handler.
    const std::uint64_t one = 1;
    const ssize_t written = ::write(stop_event_.get(), &one, sizeof(one));
    static_cast<void>(written);
}

void Server::run() {
    std::array<epoll_event, 64> ready{};
    bool stop_requested = false;
    if (pipeline_) {
        pipeline_->start();
    }

    while (!stop_requested) {
        // Normally the thread sleeps here until something happens.
        const int timeout = must_not_sleep() ? 0 : -1;
        const int count =
            ::epoll_wait(epoll_.get(), ready.data(), static_cast<int>(ready.size()), timeout);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("epoll_wait");
        }

        for (const epoll_event& event : std::span(ready).first(static_cast<std::size_t>(count))) {
            const std::uint64_t token = event.data.u64;
            if (token == stop_token) {
                stop_requested = true;
            } else if (token == listener_token) {
                accept_connections();
            } else {
                on_ready(token, event.events);
            }
        }
        drain_engine_events();
        retry_held_commands();
        finish_iteration();
    }

    shut_down();
}

bool Server::must_not_sleep() const noexcept {
    // Commands held back by a full queue must be retried whether or not a
    // socket has news.
    if (commands_held_back() || config_.network_wait == NetworkWait::Spin) {
        return true;
    }
    // Pipeline mode: the engine thread still owes answers. They are a few
    // microseconds away, much less than the kernel takes to put this thread
    // to sleep and wake it up again. Nothing would wake it up, either: the
    // engine thread does not signal its events, it relies on this rule.
    return commands_answered_ < commands_submitted_;
}

void Server::shut_down() {
    shutting_down_ = true;
    // Nothing more is accepted or read: only what is already in the server
    // gets finished. Each connection stays registered with epoll, with no
    // event of interest for now.
    ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, listener_.get(), nullptr);
    for (auto& [session_id, connection] : connections_) {
        connection.waiting_for_writable = false;
        epoll_control(epoll_, EPOLL_CTL_MOD, connection.socket.get(), 0, session_id);
    }
    deferred_commands_.clear();
    stalled_.clear();

    if (pipeline_) {
        // Let the engine thread finish every command it was given, taking its
        // events as they come so that it never waits for room.
        while (pipeline_->processed() < commands_submitted_) {
            drain_engine_events();
            cpu_relax();
        }
        drain_engine_events();
        pipeline_->stop();
    }

    send_remaining_output();
    connections_.clear();
    dirty_.clear();
    closing_.clear();
}

void Server::send_remaining_output() {
    const auto deadline = std::chrono::steady_clock::now() + config_.shutdown_grace;
    std::array<epoll_event, 64> ready{};
    while (true) {
        bool output_left = false;
        for (auto& [session_id, connection] : connections_) {
            if (connection.closing || connection.output.empty()) {
                continue;
            }
            flush(connection);
            output_left = output_left || (!connection.closing && !connection.output.empty());
        }
        if (!output_left) {
            return;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            // Clients that still have not read their messages are abandoned.
            return;
        }
        // Sleeps until a socket can take more output, or the grace period ends.
        const int count = ::epoll_wait(epoll_.get(), ready.data(), static_cast<int>(ready.size()),
                                       static_cast<int>(remaining.count()));
        if (count < 0 && errno != EINTR) {
            return;
        }
    }
}

void Server::on_ready(SessionId session_id, std::uint32_t ready_events) {
    const auto it = connections_.find(session_id);
    if (it == connections_.end() || it->second.closing) {
        return;
    }
    Connection& connection = it->second;
    // While commands are held back, nothing more is read from any socket:
    // unread data stays in the kernel, which slows the senders down (TCP flow
    // control) instead of letting it pile up in this process.
    if ((ready_events & EPOLLIN) != 0U && !commands_held_back()) {
        on_readable(connection);
    }
    if ((ready_events & EPOLLOUT) != 0U && !connection.closing) {
        flush(connection);
    }
    // An error or hang-up with nothing left to read. When data is still
    // readable, the read path sees the end of the stream itself.
    if ((ready_events & (EPOLLERR | EPOLLHUP)) != 0U && (ready_events & EPOLLIN) == 0U) {
        schedule_close(connection);
    }
}

void Server::accept_connections() {
    while (true) {
        net::FileDescriptor socket(
            ::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (!socket.valid()) {
            // EAGAIN: every waiting connection has been accepted. Other errors
            // concern one connection attempt or a temporary lack of resources;
            // the listener stays registered, so accepting is tried again later.
            return;
        }
        net::set_tcp_no_delay(socket);
        if (config_.socket_send_buffer_bytes > 0) {
            ::setsockopt(socket.get(), SOL_SOCKET, SO_SNDBUF, &config_.socket_send_buffer_bytes,
                         sizeof(config_.socket_send_buffer_bytes));
        }

        const SessionId session_id = next_session_id_++;
        epoll_control(epoll_, EPOLL_CTL_ADD, socket.get(), EPOLLIN, session_id);
        Connection& connection = connections_[session_id];
        connection.socket = std::move(socket);
        connection.session_id = session_id;
    }
}

void Server::on_readable(Connection& connection) {
    const ssize_t received =
        ::recv(connection.socket.get(), read_buffer_.data(), read_buffer_.size(), 0);
    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            schedule_close(connection);
        }
        return;
    }
    if (received == 0) {
        // The client closed its side of the connection.
        schedule_close(connection);
        return;
    }
    const auto chunk = std::span(read_buffer_).first(static_cast<std::size_t>(received));
    connection.input.insert(connection.input.end(), chunk.begin(), chunk.end());

    process_input(connection);
}

void Server::process_input(Connection& connection) {
    // The buffer can hold several messages, or only the beginning of one.
    std::size_t consumed = 0;
    while (!connection.closing) {
        const protocol::Decoded<protocol::ClientMessage> decoded =
            protocol::decode_client_message(std::span(connection.input).subspan(consumed));
        if (decoded.status == protocol::DecodeStatus::Incomplete) {
            break;
        }
        if (decoded.status == protocol::DecodeStatus::Invalid) {
            schedule_close(connection, decoded.error);
            break;
        }
        if (!handle_message(connection, decoded.message)) {
            // The command queue is full. The message stays in the buffer and
            // is handled again later.
            if (!connection.stalled) {
                connection.stalled = true;
                stalled_.push_back(connection.session_id);
            }
            break;
        }
        consumed += decoded.size;
    }
    connection.input.erase(connection.input.begin(),
                           connection.input.begin() + static_cast<std::ptrdiff_t>(consumed));
}

bool Server::handle_message(Connection& connection, const protocol::ClientMessage& message) {
    const SessionId session_id = connection.session_id;

    if (const auto* hello = std::get_if<protocol::Hello>(&message)) {
        if (connection.hello_received) {
            schedule_close(connection, protocol::ProtocolErrorCode::HelloRequired);
        } else if (hello->magic != protocol::magic) {
            schedule_close(connection, protocol::ProtocolErrorCode::BadMagic);
        } else if (hello->version != protocol::version) {
            schedule_close(connection, protocol::ProtocolErrorCode::UnsupportedVersion);
        } else {
            connection.hello_received = true;
            send(session_id,
                 protocol::HelloAck{.version = protocol::version, .session_id = session_id});
        }
        return true;
    }
    if (!connection.hello_received) {
        schedule_close(connection, protocol::ProtocolErrorCode::HelloRequired);
        return true;
    }

    if (const auto* order = std::get_if<protocol::NewOrder>(&message)) {
        return submit(NewOrder{.session_id = session_id,
                               .client_order_id = order->client_order_id,
                               .side = order->side,
                               .price = order->price,
                               .quantity = order->quantity});
    }
    if (const auto* cancel = std::get_if<protocol::CancelOrder>(&message)) {
        return submit(CancelOrder{.session_id = session_id, .order_id = cancel->order_id});
    }
    return submit(SnapshotRequest{.session_id = session_id});
}

bool Server::submit(const Command& command) {
    if (!pipeline_) {
        events_.clear();
        engine_.process(command, events_);
        for (const Event& event : events_) {
            publish(event);
        }
        return true;
    }
    // Commands the server deferred earlier go first.
    if (!deferred_commands_.empty() || !pipeline_->try_submit(command)) {
        return false;
    }
    ++commands_submitted_;
    return true;
}

void Server::submit_or_defer(const Command& command) {
    if (!submit(command)) {
        deferred_commands_.push_back(command);
    }
}

void Server::publish(const Event& event) {
    publisher_.publish(event, [this](SessionId session_id, const protocol::ServerMessage& message) {
        return send(session_id, message);
    });
}

void Server::drain_engine_events() {
    if (!pipeline_) {
        return;
    }
    // Read before emptying the queue: the engine thread queues the events of a
    // command before it counts the command, so every event of the commands
    // counted here is taken by the loop below.
    const std::uint64_t processed = pipeline_->processed();
    Event event;
    while (pipeline_->try_take(event)) {
        publish(event);
    }
    commands_answered_ = processed;
}

void Server::retry_held_commands() {
    while (!deferred_commands_.empty() && pipeline_->try_submit(deferred_commands_.front())) {
        deferred_commands_.pop_front();
        ++commands_submitted_;
    }
    if (!deferred_commands_.empty()) {
        return;
    }
    // Connections are retried in the order they stalled. A connection that
    // stalls again puts itself back on the list.
    const std::vector<SessionId> stalled = std::exchange(stalled_, {});
    for (const SessionId session_id : stalled) {
        const auto it = connections_.find(session_id);
        if (it == connections_.end()) {
            continue;
        }
        it->second.stalled = false;
        process_input(it->second);
    }
}

bool Server::send(SessionId session_id, const protocol::ServerMessage& message) {
    const auto it = connections_.find(session_id);
    if (it == connections_.end() || it->second.closing) {
        return false;
    }
    Connection& connection = it->second;
    protocol::encode(message, connection.output);

    if (connection.output.size() > config_.max_output_bytes) {
        // A lot is waiting for this client. That alone proves nothing: a large
        // snapshot is produced all at once. Offer it to the kernel first.
        flush(connection);
        if (connection.closing) {
            return false;
        }
        if (connection.output.size() > config_.max_output_bytes) {
            // The kernel would not take it either: the client is not reading
            // fast enough. Its messages are never dropped one by one: the
            // whole session ends instead (SPEC section 10.1).
            schedule_close(connection);
            return false;
        }
    }
    if (!connection.dirty) {
        connection.dirty = true;
        dirty_.push_back(session_id);
    }
    return true;
}

void Server::flush(Connection& connection) {
    std::size_t sent_total = 0;
    while (sent_total < connection.output.size()) {
        // MSG_NOSIGNAL: writing to a closed connection returns an error
        // instead of killing the process with SIGPIPE.
        const ssize_t sent = ::send(connection.socket.get(), connection.output.data() + sent_total,
                                    connection.output.size() - sent_total, MSG_NOSIGNAL);
        if (sent > 0) {
            sent_total += static_cast<std::size_t>(sent);
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // The kernel's buffer for this socket is full. Keep the rest and
            // ask epoll to report when there is room again.
            break;
        }
        schedule_close(connection);
        return;
    }
    connection.output.erase(connection.output.begin(),
                            connection.output.begin() + static_cast<std::ptrdiff_t>(sent_total));
    watch_writable(connection, !connection.output.empty());
}

void Server::watch_writable(Connection& connection, bool enable) {
    if (connection.waiting_for_writable == enable) {
        return;
    }
    connection.waiting_for_writable = enable;
    // While shutting down, input is no longer read.
    const std::uint32_t input = shutting_down_ ? 0U : static_cast<std::uint32_t>(EPOLLIN);
    const std::uint32_t events = enable ? (input | EPOLLOUT) : input;
    epoll_control(epoll_, EPOLL_CTL_MOD, connection.socket.get(), events, connection.session_id);
}

void Server::schedule_close(Connection& connection,
                            std::optional<protocol::ProtocolErrorCode> error) {
    if (connection.closing) {
        return;
    }
    // Closing is deferred to the end of the loop iteration, because the code
    // that detects the problem is often still using the connection.
    connection.closing = true;
    connection.close_error = error;
    closing_.push_back(connection.session_id);
}

void Server::finish_iteration() {
    // Closing a session cancels its orders, which can produce messages for
    // other sessions, hence the loop.
    while (!dirty_.empty() || !closing_.empty()) {
        const std::vector<SessionId> dirty = std::exchange(dirty_, {});
        for (const SessionId session_id : dirty) {
            const auto it = connections_.find(session_id);
            if (it == connections_.end()) {
                continue;
            }
            it->second.dirty = false;
            if (!it->second.closing) {
                flush(it->second);
            }
        }
        close_scheduled_connections();
    }
}

void Server::close_scheduled_connections() {
    const std::vector<SessionId> closing = std::exchange(closing_, {});
    for (const SessionId session_id : closing) {
        const auto it = connections_.find(session_id);
        if (it == connections_.end()) {
            continue;
        }
        Connection& connection = it->second;
        if (connection.close_error.has_value()) {
            // Best effort: send what was already queued, then the reason, if
            // the socket accepts it now.
            protocol::encode(protocol::ProtocolError{.code = *connection.close_error},
                             connection.output);
            const ssize_t sent = ::send(connection.socket.get(), connection.output.data(),
                                        connection.output.size(), MSG_NOSIGNAL);
            static_cast<void>(sent);
        }
        const bool had_session = connection.hello_received;
        // Closing the descriptor also removes it from epoll.
        connections_.erase(it);
        publisher_.remove_session(session_id);

        if (had_session) {
            // Cancels the session's orders (SPEC section 4.3).
            submit_or_defer(SessionClosed{.session_id = session_id});
        }
    }
}

} // namespace exchange::server
