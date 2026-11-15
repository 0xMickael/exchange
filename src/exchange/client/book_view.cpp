#include "exchange/client/book_view.hpp"

#include <algorithm>

namespace exchange::client {

namespace {

constexpr std::size_t column_width = 10;

std::string right_aligned(const std::string& text) {
    return text.size() >= column_width ? text : std::string(column_width - text.size(), ' ') + text;
}

std::string cell(const std::vector<LevelInfo>& levels, std::size_t row, bool price_first) {
    if (row >= levels.size()) {
        std::string blank(2 * column_width, ' ');
        return blank;
    }
    const std::string price = right_aligned(std::to_string(levels[row].price));
    const std::string quantity = right_aligned(std::to_string(levels[row].total_quantity));
    return price_first ? price + quantity : quantity + price;
}

} // namespace

void BookView::begin_snapshot() {
    bids_.clear();
    asks_.clear();
    ready_ = false;
}

void BookView::set_level(Side side, Price price, Quantity total_quantity) {
    std::map<Price, Quantity>& levels = side == Side::Buy ? bids_ : asks_;
    if (total_quantity == 0) {
        levels.erase(price);
    } else {
        levels[price] = total_quantity;
    }
}

std::vector<LevelInfo> BookView::levels(Side side) const {
    const std::map<Price, Quantity>& levels = side == Side::Buy ? bids_ : asks_;
    std::vector<LevelInfo> result;
    result.reserve(levels.size());
    for (const auto& [price, total] : levels) {
        result.push_back(LevelInfo{.price = price, .total_quantity = total});
    }
    if (side == Side::Buy) {
        std::ranges::reverse(result);
    }
    return result;
}

std::vector<std::string> BookView::render(std::size_t depth) const {
    const std::vector<LevelInfo> bids = levels(Side::Buy);
    const std::vector<LevelInfo> asks = levels(Side::Sell);
    if (bids.empty() && asks.empty()) {
        return {"the book is empty"};
    }

    std::vector<std::string> lines;
    lines.push_back(right_aligned("") + right_aligned("BIDS") + " | " + right_aligned("ASKS"));
    lines.push_back(right_aligned("quantity") + right_aligned("price") + " | " +
                    right_aligned("price") + right_aligned("quantity"));
    const std::size_t rows = std::min(depth, std::max(bids.size(), asks.size()));
    for (std::size_t row = 0; row < rows; ++row) {
        std::string line = cell(bids, row, false) + " | " + cell(asks, row, true);
        // No trailing spaces when the ask side has fewer levels.
        line.erase(line.find_last_not_of(' ') + 1);
        lines.push_back(std::move(line));
    }
    const std::size_t hidden_bids = bids.size() > depth ? bids.size() - depth : 0;
    const std::size_t hidden_asks = asks.size() > depth ? asks.size() - depth : 0;
    if (hidden_bids + hidden_asks > 0) {
        lines.push_back("(not shown: " + std::to_string(hidden_bids) + " bid levels, " +
                        std::to_string(hidden_asks) + " ask levels)");
    }
    return lines;
}

} // namespace exchange::client
