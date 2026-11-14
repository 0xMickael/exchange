#include "exchange/core/text.hpp"

#include <variant>

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

} // namespace exchange
