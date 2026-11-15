#pragma once

#include "exchange/client/book_view.hpp"
#include "exchange/core/types.hpp"
#include "exchange/protocol/messages.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace exchange::client {

// What the client must do after an input: messages to send to the server,
// lines to show to the user, and whether to stop.
struct CliOutput {
    std::vector<protocol::ClientMessage> to_send;
    std::vector<std::string> lines;
    bool quit = false;
};

// The logic of the command-line client, with no I/O: it is given the lines the
// user types and the messages the server sends, and answers with a CliOutput.
// Reading the terminal and the socket is the job of run_cli().
class CliSession {
public:
    // The first message of the conversation.
    [[nodiscard]] static CliOutput start();

    // Handles one line typed by the user.
    [[nodiscard]] CliOutput on_line(std::string_view line);

    // Handles one message received from the server.
    [[nodiscard]] CliOutput on_message(const protocol::ServerMessage& message);

    // Returns a request whose answer arrives after the answers to everything
    // sent before it (SPEC section 7.5), and starts waiting for that answer.
    [[nodiscard]] protocol::ClientMessage begin_sync();
    [[nodiscard]] bool sync_pending() const noexcept { return sync_pending_; }

    // True once the handshake is done and the first book snapshot is complete.
    [[nodiscard]] bool ready() const noexcept { return book_.ready(); }

private:
    struct OrderDetails {
        Side side;
        Price price;
        Quantity quantity; // Remaining quantity once the order is accepted.
    };

    void on(const protocol::HelloAck& message, CliOutput& output);
    void on(const protocol::OrderAccepted& message, CliOutput& output);
    void on(const protocol::OrderRejected& message, CliOutput& output);
    void on(const protocol::Fill& message, CliOutput& output);
    void on(const protocol::OrderCancelled& message, CliOutput& output);
    void on(const protocol::CancelRejected& message, CliOutput& output);
    void on(const protocol::Trade& message, CliOutput& output);
    void on(const protocol::BookUpdate& message, CliOutput& output);
    void on(const protocol::SnapshotBegin& message, CliOutput& output);
    void on(const protocol::SnapshotLevel& message, CliOutput& output);
    void on(const protocol::SnapshotEnd& message, CliOutput& output);
    void on(const protocol::ProtocolError& message, CliOutput& output);

    void submit_order(Side side, const std::vector<std::string_view>& words, CliOutput& output);
    // Checks a market data sequence number; asks for a new snapshot on a gap.
    bool accept_sequence(std::uint64_t sequence, CliOutput& output);

    ClientOrderId next_client_order_id_ = 1;
    // Orders sent and not yet answered, by client order id.
    std::map<ClientOrderId, OrderDetails> unanswered_orders_;
    // This session's active orders, by order id.
    std::map<OrderId, OrderDetails> active_orders_;
    BookView book_;
    std::uint64_t last_sequence_ = 0;
    bool sync_pending_ = false;
};

} // namespace exchange::client
