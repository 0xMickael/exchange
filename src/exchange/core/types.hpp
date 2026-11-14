#pragma once

#include <cstdint>

namespace exchange {

enum class Side : std::uint8_t { Buy, Sell };

// Prices are integer ticks and quantities integer units (SPEC section 2).
using Price = std::int64_t;
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;
using ClientOrderId = std::uint64_t;
using SessionId = std::uint64_t;

inline constexpr Price min_price = 1;
inline constexpr Price max_price = 1'000'000'000;
inline constexpr Quantity min_quantity = 1;
inline constexpr Quantity max_quantity = 1'000'000'000;

constexpr bool is_valid_price(Price price) noexcept {
    return price >= min_price && price <= max_price;
}

constexpr bool is_valid_quantity(Quantity quantity) noexcept {
    return quantity >= min_quantity && quantity <= max_quantity;
}

constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

} // namespace exchange
