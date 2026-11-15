#include "exchange/client/cli.hpp"

#include "exchange/client/cli_session.hpp"
#include "exchange/net/tcp.hpp"
#include "exchange/protocol/codec.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <optional>
#include <ostream>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace exchange::client {

namespace {

// Splits what is read from a file descriptor into lines.
class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    // Reads what is available. Returns false at the end of the input.
    bool fill() {
        std::array<char, 4096> buffer{};
        const ssize_t count = ::read(fd_, buffer.data(), buffer.size());
        if (count > 0) {
            buffer_.append(buffer.data(), static_cast<std::size_t>(count));
            return true;
        }
        if (count < 0 && errno == EINTR) {
            return true;
        }
        at_end_ = true;
        return false;
    }

    // The next complete line, without its line break. At the end of the
    // input, a last line with no line break is returned too.
    std::optional<std::string> next() {
        const std::size_t line_break = buffer_.find('\n');
        if (line_break != std::string::npos) {
            std::string line = buffer_.substr(0, line_break);
            buffer_.erase(0, line_break + 1);
            return line;
        }
        if (at_end_ && !buffer_.empty()) {
            return std::exchange(buffer_, {});
        }
        return std::nullopt;
    }

    [[nodiscard]] bool at_end() const noexcept { return at_end_; }

private:
    int fd_;
    std::string buffer_;
    bool at_end_ = false;
};

bool send_all(const net::FileDescriptor& socket, std::span<const std::byte> bytes) {
    std::size_t sent_total = 0;
    while (sent_total < bytes.size()) {
        const ssize_t sent = ::send(socket.get(), bytes.data() + sent_total,
                                    bytes.size() - sent_total, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        sent_total += static_cast<std::size_t>(sent);
    }
    return true;
}

// The running client: its connection, its state and how it talks to the user.
class Cli {
public:
    Cli(net::FileDescriptor socket, int input, std::ostream& output)
        : socket_(std::move(socket)), input_(input), lines_(input), output_(output),
          interactive_(::isatty(input) == 1) {}

    int run() {
        if (!apply(CliSession::start())) {
            return fail("cannot send to the server");
        }
        while (true) {
            if (accepting_input()) {
                if (const std::optional<int> status = process_input_lines()) {
                    return *status;
                }
            }
            if (accepting_input()) {
                show_prompt();
            }
            if (const std::optional<int> status = wait_for_activity()) {
                return *status;
            }
        }
    }

private:
    // The user's next command is only read once the previous one has been
    // answered, and not before the first book snapshot has arrived.
    [[nodiscard]] bool accepting_input() const {
        return session_.ready() && !session_.sync_pending();
    }

    // Handles buffered input lines until one of them needs an answer from the
    // server. Returns an exit status if the client must stop.
    std::optional<int> process_input_lines() {
        while (accepting_input()) {
            const std::optional<std::string> line = lines_.next();
            if (!line.has_value()) {
                if (lines_.at_end()) {
                    return 0;
                }
                break;
            }
            if (!interactive_) {
                // A terminal shows what the user types; a script does not.
                output_ << "> " << *line << '\n';
            }
            prompt_shown_ = false;
            const CliOutput result = session_.on_line(*line);
            if (result.quit) {
                return 0;
            }
            const bool sent = !result.to_send.empty();
            if (!apply(result)) {
                return fail("cannot send to the server");
            }
            if (sent && !send_message(session_.begin_sync())) {
                return fail("cannot send to the server");
            }
        }
        return std::nullopt;
    }

    // Sleeps until the server or the user has something for us.
    std::optional<int> wait_for_activity() {
        std::array<pollfd, 2> descriptors{};
        descriptors[0] = pollfd{.fd = socket_.get(), .events = POLLIN, .revents = 0};
        // A negative descriptor makes poll() ignore the entry.
        const bool watch_input = accepting_input() && !lines_.at_end();
        descriptors[1] = pollfd{.fd = watch_input ? input_ : -1, .events = POLLIN, .revents = 0};

        if (::poll(descriptors.data(), descriptors.size(), -1) < 0) {
            return errno == EINTR ? std::nullopt : std::optional<int>(fail("poll failed"));
        }
        if (descriptors[0].revents != 0) {
            if (const std::optional<int> status = read_from_server()) {
                return status;
            }
        }
        if (descriptors[1].revents != 0) {
            lines_.fill();
        }
        return std::nullopt;
    }

    std::optional<int> read_from_server() {
        std::array<std::byte, 4096> buffer{};
        const ssize_t received = ::recv(socket_.get(), buffer.data(), buffer.size(), 0);
        if (received < 0 && errno == EINTR) {
            return std::nullopt;
        }
        if (received <= 0) {
            return fail("the server closed the connection");
        }
        const auto chunk = std::span(buffer).first(static_cast<std::size_t>(received));
        pending_.insert(pending_.end(), chunk.begin(), chunk.end());

        std::size_t consumed = 0;
        while (true) {
            const protocol::Decoded<protocol::ServerMessage> decoded =
                protocol::decode_server_message(std::span(pending_).subspan(consumed));
            if (decoded.status == protocol::DecodeStatus::Incomplete) {
                break;
            }
            if (decoded.status == protocol::DecodeStatus::Invalid) {
                return fail("the server sent an invalid message");
            }
            consumed += decoded.size;
            if (!apply(session_.on_message(decoded.message))) {
                return fail("cannot send to the server");
            }
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
        return std::nullopt;
    }

    // Shows the lines of a result and sends its messages.
    bool apply(const CliOutput& result) {
        if (!result.lines.empty() && prompt_shown_) {
            // Erase the prompt, so that a message arriving while the user is
            // idle does not end up on the same line.
            output_ << "\r\033[K";
            prompt_shown_ = false;
        }
        for (const std::string& line : result.lines) {
            output_ << line << '\n';
        }
        output_.flush();
        return std::ranges::all_of(result.to_send, [this](const protocol::ClientMessage& message) {
            return send_message(message);
        });
    }

    bool send_message(const protocol::ClientMessage& message) {
        std::vector<std::byte> bytes;
        protocol::encode(message, bytes);
        return send_all(socket_, bytes);
    }

    void show_prompt() {
        if (interactive_ && !prompt_shown_) {
            output_ << "> " << std::flush;
            prompt_shown_ = true;
        }
    }

    int fail(const char* reason) {
        if (prompt_shown_) {
            output_ << '\n';
        }
        output_ << "error: " << reason << '\n';
        return 1;
    }

    net::FileDescriptor socket_;
    int input_;
    LineReader lines_;
    std::ostream& output_;
    bool interactive_;
    bool prompt_shown_ = false;
    CliSession session_;
    std::vector<std::byte> pending_;
};

} // namespace

int run_cli(const CliOptions& options, int input, std::ostream& output) {
    net::FileDescriptor socket;
    try {
        socket = net::connect_tcp(options.address, options.port);
    } catch (const std::system_error& error) {
        output << "error: cannot connect to " << options.address << ':' << options.port << " ("
               << error.code().message() << ")\n";
        return 1;
    }
    return Cli(std::move(socket), input, output).run();
}

} // namespace exchange::client
