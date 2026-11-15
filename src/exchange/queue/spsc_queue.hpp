#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace exchange {

// A bounded queue for exactly one producer thread and one consumer thread
// (SPSC: single producer, single consumer). See SPEC section 9.1.
//
// It is a ring buffer: a fixed array of slots and two positions.
//
//   tail_  where the producer writes next. Only the producer modifies it.
//   head_  where the consumer reads next.  Only the consumer modifies it.
//
// Both positions only ever increase; the slot for position p is p modulo the
// capacity. The queue holds tail_ - head_ elements: it is empty when they are
// equal and full when they differ by the capacity.
//
// No lock is needed because each variable has a single writer. Each thread
// still has to read the other thread's position, which is why both positions
// are atomic. The comments in try_push() and try_pop() explain the memory
// orderings.
//
// Neither operation ever blocks, allocates or makes a system call.
template <typename T>
    requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class SpscQueue {
public:
    // The capacity is `min_capacity` rounded up to a power of two, so that the
    // slot of a position can be computed with a bit mask instead of a division.
    explicit SpscQueue(std::size_t min_capacity)
        : mask_(std::bit_ceil(std::max<std::size_t>(min_capacity, 1)) - 1), slots_(mask_ + 1) {}

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Appends a copy of `value`. Returns false, and changes nothing, if the
    // queue is full. Must only be called by the producer thread.
    [[nodiscard]] bool try_push(const T& value) noexcept {
        // relaxed: tail_ is only written by this thread, so this thread
        // already knows its latest value.
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // acquire: pairs with the release store of head_ in try_pop(). It
        // guarantees that the consumer has finished reading every slot before
        // head, so those slots can be overwritten.
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head == capacity()) {
            return false;
        }
        slots_[tail & mask_] = value;
        // release: publishes the slot. A consumer that observes the new tail
        // with an acquire load is guaranteed to also observe the value just
        // written to the slot.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Removes the oldest element and copies it into `value`. Returns false,
    // and leaves `value` unchanged, if the queue is empty. Must only be called
    // by the consumer thread.
    [[nodiscard]] bool try_pop(T& value) noexcept {
        // relaxed: head_ is only written by this thread.
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // acquire: pairs with the release store of tail_ in try_push(). Every
        // slot before tail is completely written.
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        value = slots_[head & mask_];
        // release: tells the producer that this slot has been read and can be
        // reused.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    // Size of a cache line on x86-64 and most ARM64 processors. The standard
    // constant std::hardware_destructive_interference_size is not used because
    // GCC warns that its value can change with compiler options.
    static constexpr std::size_t cache_line_size = 64;

    const std::size_t mask_;
    // Allocated once in the constructor and never resized.
    std::vector<T> slots_;

    // The two positions are written by different threads. Each is placed on a
    // cache line of its own: if they shared one, every write by one thread
    // would invalidate that line in the other thread's core, although the two
    // threads never touch the same variable. This is called false sharing.
    alignas(cache_line_size) std::atomic<std::size_t> head_{0};
    alignas(cache_line_size) std::atomic<std::size_t> tail_{0};
};

} // namespace exchange
