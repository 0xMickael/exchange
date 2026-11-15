#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <mutex>
#include <vector>

namespace exchange::bench {

// The same ring buffer as SpscQueue, made thread-safe in the most ordinary
// way: one mutex around every operation. It is the point of comparison that
// tells whether the lock-free queue is worth its extra subtlety.
template <typename T> class MutexQueue {
public:
    explicit MutexQueue(std::size_t min_capacity)
        : slots_(std::bit_ceil(std::max<std::size_t>(min_capacity, 1))) {}

    [[nodiscard]] bool try_push(const T& value) {
        const std::scoped_lock lock(mutex_);
        if (tail_ - head_ == slots_.size()) {
            return false;
        }
        slots_[tail_ & (slots_.size() - 1)] = value;
        ++tail_;
        return true;
    }

    [[nodiscard]] bool try_pop(T& value) {
        const std::scoped_lock lock(mutex_);
        if (head_ == tail_) {
            return false;
        }
        value = slots_[head_ & (slots_.size() - 1)];
        ++head_;
        return true;
    }

private:
    std::mutex mutex_;
    std::vector<T> slots_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
};

} // namespace exchange::bench
