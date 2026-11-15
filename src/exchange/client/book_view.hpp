#pragma once

#include "exchange/core/order_book.hpp"
#include "exchange/core/types.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace exchange::client {

// A client's copy of the order book, rebuilt from market data as described in
// SPEC section 6.5: start from the levels of a snapshot, then replace the total
// of a level each time an update names it.
class BookView {
public:
    // Forgets the current content: a snapshot is about to be received.
    void begin_snapshot();
    // Sets the total of a level; 0 removes it. Used for both snapshot levels
    // and later updates.
    void set_level(Side side, Price price, Quantity total_quantity);
    void end_snapshot() noexcept { ready_ = true; }

    // True once a whole snapshot has been received.
    [[nodiscard]] bool ready() const noexcept { return ready_; }

    // All levels of a side, best price first.
    [[nodiscard]] std::vector<LevelInfo> levels(Side side) const;

    // A table of the best `depth` levels of each side, as lines of text.
    [[nodiscard]] std::vector<std::string> render(std::size_t depth) const;

private:
    std::map<Price, Quantity> bids_;
    std::map<Price, Quantity> asks_;
    bool ready_ = false;
};

} // namespace exchange::client
