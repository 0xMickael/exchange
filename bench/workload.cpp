#include "workload.hpp"

#include "exchange/core/event.hpp"
#include "exchange/core/matching_engine.hpp"

#include <random>
#include <unordered_map>
#include <variant>

namespace exchange::bench {

namespace {

// Builds the command stream while running it through an engine of its own, to
// know at every step which orders are live and can be cancelled.
class Generator {
public:
    explicit Generator(const WorkloadConfig& config) : config_(config), rng_(config.seed) {
        last_client_order_id_.resize(config.sessions + 1, 0);
    }

    Workload run() {
        Workload workload;
        workload.setup.reserve(config_.initial_orders);
        workload.measured.reserve(config_.commands);

        for (std::size_t i = 0; i < config_.initial_orders; ++i) {
            workload.setup.push_back(passive_order());
            apply(workload.setup.back());
        }

        stats_ = {};
        for (std::size_t i = 0; i < config_.commands; ++i) {
            workload.measured.push_back(next_measured_command());
            apply(workload.measured.back());
        }

        stats_.final_orders = engine_.book().order_count();
        stats_.final_bid_levels = engine_.book().levels(Side::Buy).size();
        stats_.final_ask_levels = engine_.book().levels(Side::Sell).size();
        workload.stats = stats_;
        return workload;
    }

private:
    struct LiveOrder {
        OrderId id;
        SessionId session_id;
    };

    // mt19937_64 gives the same sequence everywhere; the standard distributions do not.
    std::uint64_t below(std::uint64_t bound) { return rng_() % bound; }

    Command next_measured_command() {
        // Keeps the book from draining or growing without bound when the mix
        // of adds and removals is not balanced.
        const bool book_is_thin = live_.size() < (config_.initial_orders / 2) + 1;
        const bool book_is_thick = live_.size() > (config_.initial_orders * 2) + 100;

        const std::uint64_t roll = below(100);
        if (!book_is_thin && roll < config_.aggressive_percent) {
            ++stats_.aggressive_orders;
            return aggressive_order();
        }
        const bool wants_cancel = roll < config_.aggressive_percent + config_.cancel_percent;
        if (!book_is_thin && !live_.empty() && (wants_cancel || book_is_thick)) {
            ++stats_.cancels;
            const LiveOrder& target = live_[below(live_.size())];
            return CancelOrder{.session_id = target.session_id, .order_id = target.id};
        }
        ++stats_.passive_orders;
        return passive_order();
    }

    // Rests in the book: a buy below the mid price or a sell above it.
    Command passive_order() {
        const Side side = below(2) == 0 ? Side::Buy : Side::Sell;
        const auto distance = static_cast<Price>(1 + below(config_.price_levels));
        const Price price =
            side == Side::Buy ? config_.mid_price - distance : config_.mid_price + distance;
        return new_order(side, price);
    }

    // Priced through the whole opposite side, so it trades with the best levels.
    Command aggressive_order() {
        const Side side = below(2) == 0 ? Side::Buy : Side::Sell;
        const auto reach = static_cast<Price>(config_.price_levels);
        const Price price =
            side == Side::Buy ? config_.mid_price + reach : config_.mid_price - reach;
        return new_order(side, price);
    }

    Command new_order(Side side, Price price) {
        const SessionId session = 1 + below(config_.sessions);
        return NewOrder{.session_id = session,
                        .client_order_id = ++last_client_order_id_[session],
                        .side = side,
                        .price = price,
                        .quantity = 1 + below(config_.max_quantity)};
    }

    void apply(const Command& command) {
        events_.clear();
        engine_.process(command, events_);
        stats_.events += events_.size();

        const OrderAccepted* accepted = nullptr;
        for (const Event& event : events_) {
            if (const auto* fill = std::get_if<Fill>(&event)) {
                if (fill->remaining_quantity == 0) {
                    forget(fill->order_id);
                }
            } else if (const auto* cancelled = std::get_if<OrderCancelled>(&event)) {
                forget(cancelled->order_id);
            } else if (std::holds_alternative<Trade>(event)) {
                ++stats_.trades;
            } else if (const auto* order_accepted = std::get_if<OrderAccepted>(&event)) {
                accepted = order_accepted;
            }
        }
        if (accepted != nullptr && engine_.book().find(accepted->order_id) != nullptr) {
            position_[accepted->order_id] = live_.size();
            live_.push_back(
                LiveOrder{.id = accepted->order_id, .session_id = accepted->session_id});
        }
    }

    // Removes an order from the live set in constant time, by moving the last
    // entry into its place.
    void forget(OrderId order_id) {
        const auto it = position_.find(order_id);
        if (it == position_.end()) {
            return;
        }
        const std::size_t index = it->second;
        position_.erase(it);
        if (index != live_.size() - 1) {
            live_[index] = live_.back();
            position_[live_[index].id] = index;
        }
        live_.pop_back();
    }

    WorkloadConfig config_;
    std::mt19937_64 rng_;
    MatchingEngine engine_;
    std::vector<Event> events_;
    std::vector<ClientOrderId> last_client_order_id_;
    std::vector<LiveOrder> live_;
    std::unordered_map<OrderId, std::size_t> position_;
    WorkloadStats stats_;
};

} // namespace

std::string validate(const WorkloadConfig& config) {
    if (config.sessions == 0) {
        return "sessions must be at least 1";
    }
    if (config.price_levels == 0) {
        return "price-levels must be at least 1";
    }
    if (config.cancel_percent + config.aggressive_percent > 100) {
        return "cancel-percent and aggressive-percent must not add up to more than 100";
    }
    if (!is_valid_quantity(config.max_quantity)) {
        return "max-quantity is out of range";
    }
    const auto reach = static_cast<Price>(config.price_levels);
    if (!is_valid_price(config.mid_price) || !is_valid_price(config.mid_price - reach) ||
        !is_valid_price(config.mid_price + reach)) {
        return "mid-price minus or plus price-levels is not a valid price";
    }
    return {};
}

Workload generate_workload(const WorkloadConfig& config) {
    return Generator(config).run();
}

} // namespace exchange::bench
