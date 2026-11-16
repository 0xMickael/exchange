#include "exchange/server/engine_pipeline.hpp"

#include "exchange/core/matching_engine.hpp"
#include "support/random_commands.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <vector>

namespace exchange::server {
namespace {

void take_available(EnginePipeline& pipeline, std::vector<Event>& events) {
    Event event;
    while (pipeline.try_take(event)) {
        events.push_back(event);
    }
}

// The engine thread must change nothing about what the engine computes: the
// same commands give the same events, in the same order, as a direct call.
void expect_same_events_as_direct_calls(std::size_t queue_capacity, EngineWait wait) {
    EnginePipeline pipeline(PipelineConfig{.command_queue_capacity = queue_capacity,
                                           .event_queue_capacity = queue_capacity,
                                           .wait = wait});
    pipeline.start();

    test::RandomCommands commands(
        7, test::WorkloadShape{.lowest_price = 95, .highest_price = 105, .max_quantity = 20});
    MatchingEngine direct;
    std::vector<Event> expected;
    std::vector<Event> actual;

    for (int i = 0; i < 20'000; ++i) {
        const Command command = commands.next();
        direct.process(command, expected);
        while (!pipeline.try_submit(command)) {
            // The command queue is full: make room in the event queue so that
            // the engine thread can keep going.
            take_available(pipeline, actual);
        }
        take_available(pipeline, actual);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (actual.size() < expected.size() && std::chrono::steady_clock::now() < deadline) {
        take_available(pipeline, actual);
    }
    pipeline.stop();

    ASSERT_EQ(actual.size(), expected.size());
    EXPECT_TRUE(actual == expected);
}

TEST(EnginePipeline, MatchesDirectCallsWithLargeQueues) {
    expect_same_events_as_direct_calls(4096, EngineWait::Spin);
}

TEST(EnginePipeline, MatchesDirectCallsWithQueuesThatAreAlwaysFull) {
    expect_same_events_as_direct_calls(2, EngineWait::Spin);
}

TEST(EnginePipeline, MatchesDirectCallsWhenTheEngineThreadSleeps) {
    expect_same_events_as_direct_calls(64, EngineWait::Backoff);
}

// The network thread decides whether it may sleep by comparing processed()
// with the number of commands it submitted. That is only sound if a command is
// counted after its events are queued, never before.
TEST(EnginePipeline, CountsACommandOnceItsEventsAreQueued) {
    EnginePipeline pipeline(PipelineConfig{});
    pipeline.start();
    EXPECT_EQ(pipeline.processed(), 0U);

    // A passive order produces two events: OrderAccepted and BookUpdate.
    ASSERT_TRUE(pipeline.try_submit(NewOrder{
        .session_id = 1, .client_order_id = 1, .side = Side::Buy, .price = 100, .quantity = 1}));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (pipeline.processed() < 1 && std::chrono::steady_clock::now() < deadline) {
    }
    ASSERT_EQ(pipeline.processed(), 1U);

    // Both events are available at once, with no further waiting.
    Event event;
    ASSERT_TRUE(pipeline.try_take(event));
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(event));
    ASSERT_TRUE(pipeline.try_take(event));
    EXPECT_TRUE(std::holds_alternative<BookUpdate>(event));
    EXPECT_FALSE(pipeline.try_take(event));
}

TEST(EnginePipeline, StopsEvenWhenNobodyTakesItsEvents) {
    EnginePipeline pipeline(
        PipelineConfig{.command_queue_capacity = 64, .event_queue_capacity = 1});
    pipeline.start();
    for (ClientOrderId id = 1; id <= 10; ++id) {
        static_cast<void>(pipeline.try_submit(NewOrder{.session_id = 1,
                                                       .client_order_id = id,
                                                       .side = Side::Buy,
                                                       .price = 100,
                                                       .quantity = 1}));
    }

    // The engine thread is stuck on a full event queue. stop() must still return.
    pipeline.stop();

    SUCCEED();
}

} // namespace
} // namespace exchange::server
