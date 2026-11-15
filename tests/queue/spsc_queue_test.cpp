#include "exchange/queue/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>

namespace exchange {
namespace {

static_assert(!std::is_copy_constructible_v<SpscQueue<int>>);
static_assert(!std::is_move_constructible_v<SpscQueue<int>>);

TEST(SpscQueue, CapacityIsRoundedUpToAPowerOfTwo) {
    EXPECT_EQ(SpscQueue<int>(0).capacity(), 1U);
    EXPECT_EQ(SpscQueue<int>(1).capacity(), 1U);
    EXPECT_EQ(SpscQueue<int>(2).capacity(), 2U);
    EXPECT_EQ(SpscQueue<int>(3).capacity(), 4U);
    EXPECT_EQ(SpscQueue<int>(1000).capacity(), 1024U);
    EXPECT_EQ(SpscQueue<int>(1024).capacity(), 1024U);
}

TEST(SpscQueue, StartsEmpty) {
    SpscQueue<int> queue(4);
    int value = 42;

    EXPECT_FALSE(queue.try_pop(value));
    EXPECT_EQ(value, 42) << "a failed pop must not modify its argument";
}

TEST(SpscQueue, ElementsComeOutInTheOrderTheyWentIn) {
    SpscQueue<int> queue(4);

    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_TRUE(queue.try_push(3));

    int value = 0;
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 3);
    EXPECT_FALSE(queue.try_pop(value));
}

TEST(SpscQueue, PushFailsWhenFullAndSucceedsAgainAfterAPop) {
    SpscQueue<int> queue(2);
    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));

    EXPECT_FALSE(queue.try_push(3));

    int value = 0;
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.try_push(3));
    EXPECT_FALSE(queue.try_push(4));

    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 3);
    EXPECT_FALSE(queue.try_pop(value));
}

TEST(SpscQueue, UsesEverySlot) {
    SpscQueue<int> queue(8);

    int pushed = 0;
    while (queue.try_push(pushed)) {
        ++pushed;
    }

    EXPECT_EQ(pushed, 8);
}

TEST(SpscQueue, PositionsWrapAroundTheBuffer) {
    SpscQueue<int> queue(4);
    int value = 0;

    // Many more elements than slots, with the queue never more than 3 deep.
    for (int i = 0; i < 1000; i += 3) {
        ASSERT_TRUE(queue.try_push(i));
        ASSERT_TRUE(queue.try_push(i + 1));
        ASSERT_TRUE(queue.try_push(i + 2));
        for (int expected = i; expected < i + 3; ++expected) {
            ASSERT_TRUE(queue.try_pop(value));
            ASSERT_EQ(value, expected);
        }
    }
    EXPECT_FALSE(queue.try_pop(value));
}

// Several fields derived from one number: a slot read while it is being
// written would show fields that do not belong together.
struct Message {
    std::uint64_t sequence = 0;
    std::uint64_t doubled = 0;
    std::uint64_t inverted = 0;
};

// One thread pushes a known sequence while another pops it. The consumer must
// see every message exactly once, in order and intact. Run under
// ThreadSanitizer, this test also reports any data race in the queue.
void transfer_between_two_threads(std::size_t capacity, std::uint64_t message_count) {
    SpscQueue<Message> queue(capacity);

    std::jthread producer([&queue, message_count] {
        for (std::uint64_t i = 0; i < message_count; ++i) {
            const Message message{.sequence = i, .doubled = 2 * i, .inverted = ~i};
            while (!queue.try_push(message)) {
                std::this_thread::yield();
            }
        }
    });

    std::uint64_t received = 0;
    std::uint64_t corrupted = 0;
    std::uint64_t out_of_order = 0;
    Message message;
    while (received < message_count) {
        if (!queue.try_pop(message)) {
            std::this_thread::yield();
            continue;
        }
        out_of_order += message.sequence == received ? 0U : 1U;
        const bool intact =
            message.doubled == 2 * message.sequence && message.inverted == ~message.sequence;
        corrupted += intact ? 0U : 1U;
        ++received;
    }
    producer.join();

    EXPECT_EQ(out_of_order, 0U);
    EXPECT_EQ(corrupted, 0U);
    EXPECT_FALSE(queue.try_pop(message)) << "the queue must be empty at the end";
}

TEST(SpscQueueThreads, TransfersEveryMessageInOrder) {
    transfer_between_two_threads(1024, 300'000);
}

TEST(SpscQueueThreads, WorksWhenConstantlyFullOrEmpty) {
    // A tiny capacity forces the producer and the consumer to meet at the
    // full and empty boundaries all the time.
    transfer_between_two_threads(2, 100'000);
}

TEST(SpscQueueThreads, WorksWithASingleSlot) {
    transfer_between_two_threads(1, 50'000);
}

} // namespace
} // namespace exchange
