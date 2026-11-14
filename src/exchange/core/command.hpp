#pragma once

#include "exchange/core/types.hpp"

#include <variant>

namespace exchange {

// Engine inputs (SPEC section 5.1).

struct NewOrder {
    SessionId session_id;
    ClientOrderId client_order_id;
    Side side;
    Price price;
    Quantity quantity;

    friend bool operator==(const NewOrder&, const NewOrder&) = default;
};

struct CancelOrder {
    SessionId session_id;
    OrderId order_id;

    friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

struct SessionClosed {
    SessionId session_id;

    friend bool operator==(const SessionClosed&, const SessionClosed&) = default;
};

using Command = std::variant<NewOrder, CancelOrder, SessionClosed>;

} // namespace exchange
