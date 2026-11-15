#include "exchange/core/text.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace exchange {

namespace {

using std::to_string;

std::string reason_name(RejectReason reason) {
    switch (reason) {
    case RejectReason::InvalidClientOrderId:
        return "invalid_client_order_id";
    case RejectReason::InvalidPrice:
        return "invalid_price";
    case RejectReason::InvalidQuantity:
        return "invalid_quantity";
    }
    return "unknown";
}

std::string reason_name(CancelRejectReason reason) {
    switch (reason) {
    case CancelRejectReason::UnknownOrder:
        return "unknown_order";
    }
    return "unknown";
}

std::string reason_name(CancelReason reason) {
    switch (reason) {
    case CancelReason::Requested:
        return "requested";
    case CancelReason::SessionClosed:
        return "session_closed";
    }
    return "unknown";
}

struct CommandFormatter {
    std::string operator()(const NewOrder& c) const {
        return "new " + to_string(c.session_id) + " " + to_string(c.client_order_id) + " " +
               exchange::to_string(c.side) + " " + to_string(c.price) + " " + to_string(c.quantity);
    }
    std::string operator()(const CancelOrder& c) const {
        return "cancel " + to_string(c.session_id) + " " + to_string(c.order_id);
    }
    std::string operator()(const SessionClosed& c) const {
        return "close " + to_string(c.session_id);
    }
};

struct EventFormatter {
    std::string operator()(const OrderAccepted& e) const {
        return "accepted session=" + to_string(e.session_id) +
               " client_order_id=" + to_string(e.client_order_id) +
               " order_id=" + to_string(e.order_id);
    }
    std::string operator()(const OrderRejected& e) const {
        return "rejected session=" + to_string(e.session_id) +
               " client_order_id=" + to_string(e.client_order_id) +
               " reason=" + reason_name(e.reason);
    }
    std::string operator()(const Fill& e) const {
        return "fill session=" + to_string(e.session_id) + " order_id=" + to_string(e.order_id) +
               " price=" + to_string(e.price) + " quantity=" + to_string(e.quantity) +
               " remaining=" + to_string(e.remaining_quantity);
    }
    std::string operator()(const OrderCancelled& e) const {
        return "cancelled session=" + to_string(e.session_id) +
               " order_id=" + to_string(e.order_id) +
               " quantity=" + to_string(e.cancelled_quantity) + " reason=" + reason_name(e.reason);
    }
    std::string operator()(const CancelRejected& e) const {
        return "cancel_rejected session=" + to_string(e.session_id) +
               " order_id=" + to_string(e.order_id) + " reason=" + reason_name(e.reason);
    }
    std::string operator()(const Trade& e) const {
        return "trade price=" + to_string(e.price) + " quantity=" + to_string(e.quantity) +
               " aggressor=" + exchange::to_string(e.aggressor_side);
    }
    std::string operator()(const BookUpdate& e) const {
        return "book side=" + exchange::to_string(e.side) + " price=" + to_string(e.price) +
               " total=" + to_string(e.total_quantity);
    }
};

} // namespace

std::string to_string(Side side) {
    return side == Side::Buy ? "buy" : "sell";
}

std::string to_string(const Command& command) {
    return std::visit(CommandFormatter{}, command);
}

std::string to_string(const Event& event) {
    return std::visit(EventFormatter{}, event);
}

namespace {

std::vector<std::string_view> split_words(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t position = 0;
    while (position < line.size()) {
        const std::size_t start = line.find_first_not_of(" \t", position);
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t end = std::min(line.find_first_of(" \t", start), line.size());
        words.push_back(line.substr(start, end - start));
        position = end;
    }
    return words;
}

// Parses a whole word as a decimal integer of type T. Rejects trailing
// characters and values that do not fit in T.
template <typename T> bool parse_number(std::string_view word, T& value) {
    const char* const begin = word.data();
    const char* const end = begin + word.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && parsed_end == end;
}

bool parse_side(std::string_view word, Side& side) {
    if (word == "buy") {
        side = Side::Buy;
        return true;
    }
    if (word == "sell") {
        side = Side::Sell;
        return true;
    }
    return false;
}

} // namespace

std::optional<Command> parse_command(std::string_view line) {
    const std::vector<std::string_view> words = split_words(line);
    if (words.empty()) {
        return std::nullopt;
    }

    if (words[0] == "new" && words.size() == 6) {
        NewOrder command{};
        if (parse_number(words[1], command.session_id) &&
            parse_number(words[2], command.client_order_id) && parse_side(words[3], command.side) &&
            parse_number(words[4], command.price) && parse_number(words[5], command.quantity)) {
            return command;
        }
    } else if (words[0] == "cancel" && words.size() == 3) {
        CancelOrder command{};
        if (parse_number(words[1], command.session_id) &&
            parse_number(words[2], command.order_id)) {
            return command;
        }
    } else if (words[0] == "close" && words.size() == 2) {
        SessionClosed command{};
        if (parse_number(words[1], command.session_id)) {
            return command;
        }
    }
    return std::nullopt;
}

} // namespace exchange
