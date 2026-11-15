#include "exchange/client/cli_session.hpp"

#include "exchange/core/text.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <system_error>
#include <variant>

namespace exchange::client {

namespace {

using std::to_string;

constexpr std::size_t default_book_depth = 10;

// OrderId 0 is never assigned (SPEC section 2), so cancelling it is always
// rejected: it is used as a request with no effect.
constexpr OrderId sync_order_id = 0;

const std::vector<std::string> help_lines = {
    "commands:",
    "  buy PRICE QUANTITY    send a buy order",
    "  sell PRICE QUANTITY   send a sell order",
    "  cancel ORDER_ID       cancel one of your orders",
    "  orders                list your active orders",
    "  book [DEPTH]          show the order book (10 levels per side by default)",
    "  help                  show this list",
    "  quit                  leave (your active orders are cancelled)",
    "prices are in ticks and quantities in units, both whole numbers",
};

std::vector<std::string_view> split_words(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t position = 0;
    while (position < line.size()) {
        const std::size_t start = line.find_first_not_of(" \t\r", position);
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t end = std::min(line.find_first_of(" \t\r", start), line.size());
        words.push_back(line.substr(start, end - start));
        position = end;
    }
    return words;
}

template <typename T> bool parse_number(std::string_view word, T& value) {
    const char* const begin = word.data();
    const char* const end = begin + word.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && parsed_end == end;
}

std::string describe(Side side, Price price, Quantity quantity) {
    return exchange::to_string(side) + " " + to_string(quantity) + " @ " + to_string(price);
}

std::string reason_text(RejectReason reason) {
    switch (reason) {
    case RejectReason::InvalidClientOrderId:
        return "invalid client order id";
    case RejectReason::InvalidPrice:
        return "invalid price";
    case RejectReason::InvalidQuantity:
        return "invalid quantity";
    }
    return "unknown reason";
}

std::string error_text(protocol::ProtocolErrorCode code) {
    switch (code) {
    case protocol::ProtocolErrorCode::BadLength:
        return "bad length";
    case protocol::ProtocolErrorCode::UnknownType:
        return "unknown message type";
    case protocol::ProtocolErrorCode::BadField:
        return "bad field";
    case protocol::ProtocolErrorCode::HelloRequired:
        return "hello required";
    case protocol::ProtocolErrorCode::BadMagic:
        return "bad magic";
    case protocol::ProtocolErrorCode::UnsupportedVersion:
        return "unsupported version";
    }
    return "unknown error";
}

} // namespace

CliOutput CliSession::start() {
    CliOutput output;
    output.to_send.emplace_back(
        protocol::Hello{.magic = protocol::magic, .version = protocol::version});
    return output;
}

protocol::ClientMessage CliSession::begin_sync() {
    sync_pending_ = true;
    return protocol::CancelOrder{.order_id = sync_order_id};
}

CliOutput CliSession::on_line(std::string_view line) {
    CliOutput output;
    const std::vector<std::string_view> words = split_words(line);
    if (words.empty()) {
        return output;
    }
    const std::string_view command = words[0];

    if (command == "buy" || command == "sell") {
        submit_order(command == "buy" ? Side::Buy : Side::Sell, words, output);
    } else if (command == "cancel") {
        OrderId order_id = 0;
        if (words.size() != 2 || !parse_number(words[1], order_id) || order_id == sync_order_id) {
            output.lines.emplace_back("usage: cancel ORDER_ID");
        } else {
            output.to_send.emplace_back(protocol::CancelOrder{.order_id = order_id});
        }
    } else if (command == "orders") {
        if (active_orders_.empty()) {
            output.lines.emplace_back("you have no active orders");
        }
        for (const auto& [order_id, order] : active_orders_) {
            output.lines.push_back("order " + to_string(order_id) + ": " +
                                   describe(order.side, order.price, order.quantity) +
                                   " remaining");
        }
    } else if (command == "book") {
        std::size_t depth = default_book_depth;
        if (words.size() > 2 || (words.size() == 2 && !parse_number(words[1], depth))) {
            output.lines.emplace_back("usage: book [DEPTH]");
        } else {
            output.lines = book_.render(depth);
        }
    } else if (command == "help") {
        output.lines = help_lines;
    } else if (command == "quit" || command == "exit") {
        output.quit = true;
    } else {
        output.lines.push_back("unknown command '" + std::string(command) + "', try 'help'");
    }
    return output;
}

void CliSession::submit_order(Side side, const std::vector<std::string_view>& words,
                              CliOutput& output) {
    Price price = 0;
    Quantity quantity = 0;
    if (words.size() != 3 || !parse_number(words[1], price) || !parse_number(words[2], quantity)) {
        output.lines.push_back("usage: " + std::string(words[0]) + " PRICE QUANTITY");
        return;
    }
    // Range checks are left to the server, which answers with a rejection.
    const ClientOrderId client_order_id = next_client_order_id_++;
    unanswered_orders_[client_order_id] =
        OrderDetails{.side = side, .price = price, .quantity = quantity};
    output.to_send.emplace_back(protocol::NewOrder{
        .client_order_id = client_order_id, .side = side, .price = price, .quantity = quantity});
}

CliOutput CliSession::on_message(const protocol::ServerMessage& message) {
    CliOutput output;
    std::visit([this, &output](const auto& typed_message) { this->on(typed_message, output); },
               message);
    return output;
}

// A member like the other handlers, so that all of them are called the same way.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void CliSession::on(const protocol::HelloAck& message, CliOutput& output) {
    output.lines.push_back("session " + to_string(message.session_id) + " opened");
    // Market data is needed to show the book.
    output.to_send.emplace_back(protocol::Subscribe{});
}

void CliSession::on(const protocol::OrderAccepted& message, CliOutput& output) {
    const auto it = unanswered_orders_.find(message.client_order_id);
    if (it == unanswered_orders_.end()) {
        return;
    }
    const OrderDetails order = it->second;
    unanswered_orders_.erase(it);
    active_orders_[message.order_id] = order;
    output.lines.push_back("order " + to_string(message.order_id) +
                           " accepted: " + describe(order.side, order.price, order.quantity));
}

void CliSession::on(const protocol::OrderRejected& message, CliOutput& output) {
    std::string line = "order rejected (" + reason_text(message.reason) + ")";
    const auto it = unanswered_orders_.find(message.client_order_id);
    if (it != unanswered_orders_.end()) {
        line += ": " + describe(it->second.side, it->second.price, it->second.quantity);
        unanswered_orders_.erase(it);
    }
    output.lines.push_back(std::move(line));
}

void CliSession::on(const protocol::Fill& message, CliOutput& output) {
    std::string line = "order " + to_string(message.order_id) +
                       " filled: " + to_string(message.quantity) + " @ " + to_string(message.price);
    if (message.remaining_quantity == 0) {
        line += ", complete";
        active_orders_.erase(message.order_id);
    } else {
        line += ", " + to_string(message.remaining_quantity) + " remaining";
        const auto it = active_orders_.find(message.order_id);
        if (it != active_orders_.end()) {
            it->second.quantity = message.remaining_quantity;
        }
    }
    output.lines.push_back(std::move(line));
}

void CliSession::on(const protocol::OrderCancelled& message, CliOutput& output) {
    active_orders_.erase(message.order_id);
    output.lines.push_back("order " + to_string(message.order_id) +
                           " cancelled: " + to_string(message.cancelled_quantity) + " not filled");
}

void CliSession::on(const protocol::CancelRejected& message, CliOutput& output) {
    if (message.order_id == sync_order_id && sync_pending_) {
        sync_pending_ = false;
        return;
    }
    output.lines.push_back("cancel rejected: order " + to_string(message.order_id) +
                           " is not one of your active orders");
}

void CliSession::on(const protocol::Trade& message, CliOutput& output) {
    if (!accept_sequence(message.sequence, output)) {
        return;
    }
    output.lines.push_back("market trade: " + to_string(message.quantity) + " @ " +
                           to_string(message.price) + ", " +
                           exchange::to_string(message.aggressor_side) + " side aggressor");
}

void CliSession::on(const protocol::BookUpdate& message, CliOutput& output) {
    if (!accept_sequence(message.sequence, output)) {
        return;
    }
    book_.set_level(message.side, message.price, message.total_quantity);
}

void CliSession::on(const protocol::SnapshotBegin& message, CliOutput& /*output*/) {
    book_.begin_snapshot();
    last_sequence_ = message.last_sequence;
}

void CliSession::on(const protocol::SnapshotLevel& message, CliOutput& /*output*/) {
    book_.set_level(message.side, message.price, message.total_quantity);
}

void CliSession::on(const protocol::SnapshotEnd& /*message*/, CliOutput& /*output*/) {
    book_.end_snapshot();
}

// A member like the other handlers, so that all of them are called the same way.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void CliSession::on(const protocol::ProtocolError& message, CliOutput& output) {
    output.lines.push_back("protocol error reported by the server: " + error_text(message.code));
}

bool CliSession::accept_sequence(std::uint64_t sequence, CliOutput& output) {
    if (!book_.ready()) {
        // Between a detected gap and the new snapshot: these updates are
        // already included in the snapshot that is on its way.
        return false;
    }
    if (sequence != last_sequence_ + 1) {
        output.lines.emplace_back("market data gap detected, requesting a new snapshot");
        book_.begin_snapshot();
        output.to_send.emplace_back(protocol::Subscribe{});
        return false;
    }
    last_sequence_ = sequence;
    return true;
}

} // namespace exchange::client
