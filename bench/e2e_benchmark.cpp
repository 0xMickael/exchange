#include "e2e_benchmark.hpp"

#include "latency.hpp"
#include "report.hpp"

#include "exchange/net/file_descriptor.hpp"
#include "exchange/net/tcp.hpp"
#include "exchange/protocol/codec.hpp"
#include "exchange/server/server.hpp"

#include <fcntl.h>
#include <sys/socket.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <deque>
#include <iomanip>
#include <optional>
#include <ostream>
#include <random>
#include <span>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <variant>

namespace exchange::bench {

namespace {

// One client connection, driven by its own thread. The thread never sleeps: it
// spins on a non-blocking socket, so that the time it would take the operating
// system to wake a sleeping client is not counted as server latency.
class Connection {
public:
    Connection(std::uint16_t port, const E2eConfig& config, std::size_t index,
               const BenchClock& clock)
        : socket_(net::connect_tcp("127.0.0.1", port)), workload_(config.workload),
          rng_(config.workload.seed + index), clock_(clock) {
        thin_book_ = config.workload.initial_orders / config.connections / 2;
        const int flags = ::fcntl(socket_.get(), F_GETFL);
        ::fcntl(socket_.get(), F_SETFL, flags | O_NONBLOCK);
    }

    // Opens the session and places this connection's share of the initial
    // orders, waiting for each answer. Not measured.
    bool prepare(std::size_t initial_orders) {
        if (!send(protocol::Hello{.magic = protocol::magic, .version = protocol::version})) {
            return false;
        }
        while (!hello_acknowledged_) {
            if (!receive(nullptr)) {
                return false;
            }
        }
        for (std::size_t i = 0; i < initial_orders; ++i) {
            if (!send_order(false, clock_.ticks())) {
                return false;
            }
            while (!pending_.empty()) {
                if (!receive(nullptr)) {
                    return false;
                }
            }
        }
        return true;
    }

    // Sends `requests` requests and stores the latency of each, in ticks, in
    // `samples`. With `interval_ticks` 0 the next request is sent when the
    // previous one is answered (closed loop). Otherwise request number i is
    // due at start + i * interval, whatever the answers do, and its latency is
    // counted from that due time (open loop).
    bool measure(std::size_t requests, std::uint64_t interval_ticks,
                 std::vector<std::uint64_t>& samples) {
        samples.clear();
        samples.reserve(requests);
        const std::uint64_t begin = clock_.ticks();
        std::size_t sent = 0;

        while (samples.size() < requests) {
            if (sent < requests) {
                const std::uint64_t due = begin + (sent * interval_ticks);
                const bool ready = interval_ticks == 0 ? pending_.empty() : clock_.ticks() >= due;
                if (ready) {
                    if (!send_request(interval_ticks == 0 ? clock_.ticks() : due)) {
                        return false;
                    }
                    ++sent;
                }
            }
            if (!receive(&samples)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::uint64_t cancel_rejects() const noexcept { return cancel_rejects_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    // A request that has been sent and not answered yet. Answers to one
    // session's requests come back in the order the requests were sent.
    struct Pending {
        std::uint64_t start_ticks;
        bool is_cancel;
        bool rests;       // For an order: whether it is expected to stay in the book.
        std::uint64_t id; // Client order id of an order, order id of a cancel.
    };

    std::uint64_t below(std::uint64_t bound) { return rng_() % bound; }

    // Chooses the next request following the workload mix of SPEC section 13.2.
    bool send_request(std::uint64_t start_ticks) {
        const std::uint64_t roll = below(100);
        const bool book_is_thin = live_orders_.size() <= thin_book_;
        if (!book_is_thin && roll < workload_.aggressive_percent) {
            return send_order(true, start_ticks);
        }
        if (!book_is_thin && roll < workload_.aggressive_percent + workload_.cancel_percent) {
            return send_cancel(start_ticks);
        }
        return send_order(false, start_ticks);
    }

    bool send_order(bool aggressive, std::uint64_t start_ticks) {
        const Side side = below(2) == 0 ? Side::Buy : Side::Sell;
        const auto reach = static_cast<Price>(workload_.price_levels);
        const auto distance = static_cast<Price>(1 + below(workload_.price_levels));
        // A passive buy is below the mid price; an aggressive buy is above
        // every sell. Sells mirror this.
        const Price offset = aggressive ? reach : -distance;
        const Price price =
            side == Side::Buy ? workload_.mid_price + offset : workload_.mid_price - offset;
        const ClientOrderId client_order_id = next_client_order_id_++;
        pending_.push_back(Pending{.start_ticks = start_ticks,
                                   .is_cancel = false,
                                   .rests = !aggressive,
                                   .id = client_order_id});
        return send(protocol::NewOrder{.client_order_id = client_order_id,
                                       .side = side,
                                       .price = price,
                                       .quantity = 1 + below(workload_.max_quantity)});
    }

    bool send_cancel(std::uint64_t start_ticks) {
        const std::size_t index = below(live_orders_.size());
        const OrderId order_id = live_orders_[index];
        forget(order_id);
        pending_.push_back(
            Pending{.start_ticks = start_ticks, .is_cancel = true, .rests = false, .id = order_id});
        return send(protocol::CancelOrder{.order_id = order_id});
    }

    bool send(const protocol::ClientMessage& message) {
        send_buffer_.clear();
        protocol::encode(message, send_buffer_);
        std::size_t sent_total = 0;
        while (sent_total < send_buffer_.size()) {
            const ssize_t sent = ::send(socket_.get(), send_buffer_.data() + sent_total,
                                        send_buffer_.size() - sent_total, MSG_NOSIGNAL);
            if (sent > 0) {
                sent_total += static_cast<std::size_t>(sent);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                return fail("cannot send to the server");
            }
        }
        return true;
    }

    // Reads what has arrived, without waiting, and handles every complete message.
    bool receive(std::vector<std::uint64_t>* samples) {
        std::array<std::byte, 16384> buffer{};
        const ssize_t received = ::recv(socket_.get(), buffer.data(), buffer.size(), 0);
        if (received < 0) {
            const bool nothing_yet = errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
            return nothing_yet || fail("cannot receive from the server");
        }
        if (received == 0) {
            return fail("the server closed the connection");
        }
        const std::uint64_t now = clock_.ticks();
        const auto chunk = std::span(buffer).first(static_cast<std::size_t>(received));
        received_.insert(received_.end(), chunk.begin(), chunk.end());

        std::size_t consumed = 0;
        while (true) {
            const protocol::Decoded<protocol::ServerMessage> decoded =
                protocol::decode_server_message(std::span(received_).subspan(consumed));
            if (decoded.status == protocol::DecodeStatus::Incomplete) {
                break;
            }
            if (decoded.status == protocol::DecodeStatus::Invalid) {
                return fail("the server sent an invalid message");
            }
            consumed += decoded.size;
            if (!handle(decoded.message, now, samples)) {
                return false;
            }
        }
        received_.erase(received_.begin(),
                        received_.begin() + static_cast<std::ptrdiff_t>(consumed));
        return true;
    }

    bool handle(const protocol::ServerMessage& message, std::uint64_t now,
                std::vector<std::uint64_t>* samples) {
        if (std::holds_alternative<protocol::HelloAck>(message)) {
            hello_acknowledged_ = true;
            return true;
        }
        if (const auto* fill = std::get_if<protocol::Fill>(&message)) {
            if (fill->remaining_quantity == 0) {
                forget(fill->order_id);
            }
            return true;
        }

        // Everything else is the direct answer to the oldest pending request.
        if (pending_.empty()) {
            return fail("the server sent an answer to no request");
        }
        const Pending request = pending_.front();
        bool matches = false;
        if (const auto* accepted = std::get_if<protocol::OrderAccepted>(&message)) {
            matches = !request.is_cancel && accepted->client_order_id == request.id;
            if (matches && request.rests) {
                position_[accepted->order_id] = live_orders_.size();
                live_orders_.push_back(accepted->order_id);
            }
        } else if (const auto* cancelled = std::get_if<protocol::OrderCancelled>(&message)) {
            matches = request.is_cancel && cancelled->order_id == request.id;
        } else if (const auto* rejected = std::get_if<protocol::CancelRejected>(&message)) {
            matches = request.is_cancel && rejected->order_id == request.id;
            ++cancel_rejects_;
        }
        if (!matches) {
            return fail("the server's answers do not match the requests");
        }
        pending_.pop_front();
        if (samples != nullptr) {
            samples->push_back(now - request.start_ticks);
        }
        return true;
    }

    // Removes an order from the set of orders that can be cancelled.
    void forget(OrderId order_id) {
        const auto it = position_.find(order_id);
        if (it == position_.end()) {
            return;
        }
        const std::size_t index = it->second;
        position_.erase(it);
        if (index != live_orders_.size() - 1) {
            live_orders_[index] = live_orders_.back();
            position_[live_orders_[index]] = index;
        }
        live_orders_.pop_back();
    }

    bool fail(const char* reason) {
        error_ = reason;
        return false;
    }

    net::FileDescriptor socket_;
    WorkloadConfig workload_;
    std::mt19937_64 rng_;
    const BenchClock& clock_;
    // Below this many live orders, only passive orders are sent.
    std::size_t thin_book_ = 0;

    ClientOrderId next_client_order_id_ = 1;
    std::deque<Pending> pending_;
    std::vector<OrderId> live_orders_;
    std::unordered_map<OrderId, std::size_t> position_;
    std::vector<std::byte> send_buffer_;
    std::vector<std::byte> received_;
    bool hello_acknowledged_ = false;
    std::uint64_t cancel_rejects_ = 0;
    std::string error_;
};

server::ServerConfig server_config(ServerKind kind, bool network_spin) {
    server::ServerConfig config;
    config.port = 0;
    config.network_wait = network_spin ? server::NetworkWait::Spin : server::NetworkWait::Sleep;
    config.mode =
        kind == ServerKind::Inline ? server::ServerMode::Inline : server::ServerMode::Pipeline;
    config.pipeline.wait =
        kind == ServerKind::PipelineSpin ? server::EngineWait::Spin : server::EngineWait::Backoff;
    return config;
}

// What the client threads of one run share to start measuring together.
struct RunState {
    std::atomic<std::size_t> prepared{0};
    std::atomic<std::size_t> finished{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> go{false};
};

// Sleeps until `counter` reaches `expected`. The thread that coordinates a run
// must not spin while it waits: it would keep a core busy during the
// measurement, next to the threads being measured.
void wait_until_reached(const std::atomic<std::size_t>& counter, std::size_t expected) {
    for (std::size_t seen = counter.load(); seen < expected; seen = counter.load()) {
        counter.wait(seen);
    }
}

struct ConnectionOutcome {
    std::vector<std::uint64_t> samples; // In clock ticks.
    std::uint64_t cancel_rejects = 0;
    std::string error;
};

// The body of one client thread: connect, prepare, wait for the start signal, measure.
void drive_connection(std::uint16_t port, const E2eConfig& config, std::size_t index,
                      const BenchClock& clock, std::uint64_t interval_ticks, RunState& state,
                      ConnectionOutcome& outcome) {
    try {
        Connection connection(port, config, index, clock);
        const bool ready = connection.prepare(config.workload.initial_orders / config.connections);
        if (!ready) {
            state.failed = true;
        }
        ++state.prepared;
        state.prepared.notify_one();
        while (!state.go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (ready && !state.failed &&
            !connection.measure(config.requests, interval_ticks, outcome.samples)) {
            state.failed = true;
        }
        outcome.error = connection.error();
        outcome.cancel_rejects = connection.cancel_rejects();
    } catch (const std::system_error& exception) {
        outcome.error = exception.what();
        state.failed = true;
        ++state.prepared;
        state.prepared.notify_one();
    }
    ++state.finished;
    state.finished.notify_one();
}

// One run against a new server: every connection prepares, then all of them
// measure at the same time. Returns nullopt, with `error` set, on failure.
std::optional<EngineRunResult> run_once(ServerKind kind, const E2eConfig& config,
                                        const BenchClock& clock, std::uint64_t& cancel_rejects,
                                        std::string& error) {
    server::Server server(server_config(kind, config.network_spin));
    std::jthread server_thread([&server] { server.run(); });

    const std::uint64_t interval_ticks =
        config.rate > 0.0
            ? static_cast<std::uint64_t>(std::llround(1e9 / config.rate / clock.ns_per_tick()))
            : 0;
    RunState state;
    std::vector<ConnectionOutcome> outcomes(config.connections);
    std::vector<std::jthread> clients;
    clients.reserve(config.connections);
    for (std::size_t index = 0; index < config.connections; ++index) {
        clients.emplace_back([&, index] {
            drive_connection(server.port(), config, index, clock, interval_ticks, state,
                             outcomes[index]);
        });
    }

    wait_until_reached(state.prepared, config.connections);
    const auto begin = std::chrono::steady_clock::now();
    state.go.store(true, std::memory_order_release);
    wait_until_reached(state.finished, config.connections);
    const auto end = std::chrono::steady_clock::now();
    clients.clear();
    server.request_stop();
    server_thread.join();

    std::vector<std::uint64_t> all;
    all.reserve(config.connections * config.requests);
    for (const ConnectionOutcome& outcome : outcomes) {
        if (!outcome.error.empty()) {
            error = outcome.error;
        }
        cancel_rejects += outcome.cancel_rejects;
        for (const std::uint64_t ticks : outcome.samples) {
            all.push_back(clock.to_nanoseconds(ticks));
        }
    }
    if (state.failed) {
        if (error.empty()) {
            error = "a connection failed";
        }
        return std::nullopt;
    }

    EngineRunResult run;
    run.latency_ns = summarize(all);
    const double seconds = std::chrono::duration<double>(end - begin).count();
    run.commands_per_second = seconds > 0.0 ? static_cast<double>(all.size()) / seconds : 0.0;
    return run;
}

void print_row(std::ostream& out, const std::string& server, const std::string& label,
               const EngineRunResult& run) {
    const LatencySummary& latency = run.latency_ns;
    const auto microseconds = [](std::uint64_t nanoseconds) {
        return static_cast<double>(nanoseconds) / 1000.0;
    };
    out << "  " << std::left << std::setw(19) << server << std::setw(7) << label << std::right
        << std::fixed << std::setprecision(1) << std::setw(8) << microseconds(latency.min)
        << std::setw(9) << microseconds(latency.p50) << std::setw(9) << microseconds(latency.p95)
        << std::setw(9) << microseconds(latency.p99) << std::setw(10) << microseconds(latency.p999)
        << std::setw(11) << microseconds(latency.max) << std::setw(9) << latency.mean / 1000.0
        << std::setw(12) << std::setprecision(0) << run.commands_per_second << '\n';
}

void print_json_run(std::ostream& out, const EngineRunResult& run) {
    const LatencySummary& latency = run.latency_ns;
    out << "{\"latency_ns\": {\"min\": " << latency.min << ", \"p50\": " << latency.p50
        << ", \"p95\": " << latency.p95 << ", \"p99\": " << latency.p99
        << ", \"p99_9\": " << latency.p999 << ", \"max\": " << latency.max
        << ", \"mean\": " << std::fixed << std::setprecision(1) << latency.mean
        << "}, \"requests_per_second\": " << std::setprecision(0) << run.commands_per_second << "}";
}

} // namespace

std::string to_string(ServerKind kind) {
    switch (kind) {
    case ServerKind::Inline:
        return "inline";
    case ServerKind::PipelineBackoff:
        return "pipeline (backoff)";
    case ServerKind::PipelineSpin:
        return "pipeline (spin)";
    }
    return "unknown";
}

E2eResult run_e2e_benchmark(const E2eConfig& config) {
    E2eResult result;
    const std::optional<BenchClock> clock = BenchClock::create(config.clock);
    if (!clock.has_value()) {
        result.error = "the TSC clock is not usable on this machine";
        return result;
    }
    result.clock = ClockDescription{.name = clock->name(),
                                    .ticks_per_nanosecond = 1.0 / clock->ns_per_tick(),
                                    .smallest_interval_ns = clock->smallest_interval_ns(),
                                    .reading_cost_ns = clock->reading_cost_ns()};

    for (const ServerKind kind : config.servers) {
        E2eServerResult server_result;
        server_result.kind = kind;
        for (std::uint32_t run = 0; run < config.runs; ++run) {
            const std::optional<EngineRunResult> measured =
                run_once(kind, config, *clock, server_result.cancel_rejects, result.error);
            if (!measured.has_value()) {
                return result;
            }
            server_result.runs.push_back(*measured);
        }
        result.servers.push_back(std::move(server_result));
    }
    return result;
}

void print_e2e_report(std::ostream& out, const Environment& environment, const E2eConfig& config,
                      const E2eResult& result) {
    const WorkloadConfig& workload = config.workload;
    out << "End-to-end benchmark: requests over TCP (loopback) to a server in this process\n\n";

    out << "Environment\n"
        << "  compiler     " << environment.compiler << '\n'
        << "  build        " << environment.build_type << " (" << environment.compiler_flags
        << ")\n"
        << "  system       " << environment.operating_system
        << (environment.wsl ? " [WSL2: virtualized, timings are indicative]" : "") << '\n'
        << "  cpu          " << environment.cpu_model << ", " << environment.logical_cpus
        << " logical CPUs\n\n";

    out << "Workload\n"
        << "  " << config.connections << " connection(s), " << config.requests
        << " measured requests each, seed " << workload.seed << '\n'
        << "  " << workload.initial_orders << " initial orders, prices within "
        << workload.price_levels << " ticks of " << workload.mid_price << '\n'
        << "  mix: " << workload.aggressive_percent << "% aggressive orders, "
        << workload.cancel_percent << "% cancels, the rest passive orders\n";
    if (config.rate > 0.0) {
        out << std::fixed << std::setprecision(0) << "  open loop: each connection sends "
            << config.rate << " requests per second on a fixed schedule\n"
            << "  latency is counted from the scheduled send time, so a server that falls\n"
            << "  behind is charged for the requests it kept waiting\n\n";
    } else {
        out << "  closed loop: a connection sends its next request when the previous one is\n"
            << "  answered, so a slow answer delays the following requests instead of showing\n"
            << "  up in their latency (coordinated omission); use --rate for an open loop\n\n";
    }

    out << "Method\n"
        << "  latency: from just before the request is given to the kernel until its answer\n"
        << "  has been read by the client; fills that follow the answer are not waited for\n"
        << "  client threads spin on non-blocking sockets and never sleep\n"
        << "  server network thread: "
        << (config.network_spin ? "never sleeps (--network-wait spin)" : "sleeps when idle") << '\n'
        << "  clock: " << result.clock.name << ", smallest measurable interval " << std::fixed
        << std::setprecision(1) << result.clock.smallest_interval_ns << " ns\n"
        << "  " << config.runs << " run(s) per server, each against a new server\n\n";

    out << "Latency per request in microseconds, throughput in requests per second\n"
        << "  server             run         min      p50      p95      p99     p99.9        max"
           "     mean  throughput\n";
    for (const E2eServerResult& server : result.servers) {
        const std::string name = to_string(server.kind);
        for (std::size_t i = 0; i < server.runs.size(); ++i) {
            print_row(out, name, std::to_string(i + 1), server.runs[i]);
        }
        print_row(out, name, "median", median_run(server.runs));
    }
}

void print_e2e_json(std::ostream& out, const Environment& environment, const E2eConfig& config,
                    const E2eResult& result) {
    const WorkloadConfig& workload = config.workload;
    out << "{\n  \"benchmark\": \"e2e\",\n"
        << "  \"environment\": {\"compiler\": \"" << environment.compiler
        << "\", \"build_type\": \"" << environment.build_type << "\", \"compiler_flags\": \""
        << environment.compiler_flags << "\", \"operating_system\": \""
        << environment.operating_system << "\", \"wsl\": " << (environment.wsl ? "true" : "false")
        << ", \"cpu_model\": \"" << environment.cpu_model
        << "\", \"logical_cpus\": " << environment.logical_cpus << "},\n"
        << "  \"workload\": {\"connections\": " << config.connections
        << ", \"requests_per_connection\": " << config.requests << std::fixed
        << std::setprecision(0) << ", \"rate_per_connection\": " << config.rate << ", \"loop\": \""
        << (config.rate > 0.0 ? "open" : "closed") << "\", \"seed\": " << workload.seed
        << ", \"initial_orders\": " << workload.initial_orders
        << ", \"price_levels\": " << workload.price_levels
        << ", \"cancel_percent\": " << workload.cancel_percent
        << ", \"aggressive_percent\": " << workload.aggressive_percent << "},\n"
        << "  \"method\": {\"runs\": " << config.runs << ", \"network_wait\": \""
        << (config.network_spin ? "spin" : "sleep") << "\", \"clock\": \"" << result.clock.name
        << "\", \"clock_smallest_interval_ns\": " << std::setprecision(1)
        << result.clock.smallest_interval_ns << "},\n"
        << "  \"servers\": [";
    for (std::size_t s = 0; s < result.servers.size(); ++s) {
        const E2eServerResult& server = result.servers[s];
        out << (s == 0 ? "\n" : ",\n") << "    {\"server\": \"" << to_string(server.kind)
            << "\", \"cancel_rejects\": " << server.cancel_rejects << ", \"runs\": [";
        for (std::size_t i = 0; i < server.runs.size(); ++i) {
            out << (i == 0 ? "" : ", ");
            print_json_run(out, server.runs[i]);
        }
        out << "], \"median\": ";
        print_json_run(out, median_run(server.runs));
        out << "}";
    }
    out << "\n  ]\n}\n";
}

} // namespace exchange::bench
