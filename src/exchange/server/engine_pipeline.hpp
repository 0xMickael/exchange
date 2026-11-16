#pragma once

#include "exchange/core/command.hpp"
#include "exchange/core/event.hpp"
#include "exchange/core/matching_engine.hpp"
#include "exchange/queue/spsc_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>

namespace exchange::server {

// What the engine thread does when it has no command to process.
enum class EngineWait : std::uint8_t {
    // Spin for a short while, then sleep in short naps. Uses little processor
    // time when idle, at the price of a slower reaction to the first command
    // after an idle period.
    Backoff,
    // Never sleep. Reacts as fast as possible and uses a whole core all the time.
    Spin,
};

struct PipelineConfig {
    std::size_t command_queue_capacity = 16'384;
    std::size_t event_queue_capacity = 65'536;
    EngineWait wait = EngineWait::Backoff;
};

// The matching engine on a thread of its own, between two SPSC queues
// (SPEC section 9.2, pipeline mode):
//
//   network thread --commands--> engine thread --events--> network thread
//
// The engine thread is the only one that touches the MatchingEngine. The
// network thread is the only producer of commands and the only consumer of
// events, as the queues require.
//
// The engine thread never wakes the network thread up. The network thread
// must therefore keep taking events, without sleeping, for as long as
// processed() is behind the number of commands it has submitted.
class EnginePipeline {
public:
    explicit EnginePipeline(const PipelineConfig& config);
    ~EnginePipeline();

    EnginePipeline(const EnginePipeline&) = delete;
    EnginePipeline& operator=(const EnginePipeline&) = delete;

    void start();
    // Stops the engine thread and waits for it. Commands still queued are not processed.
    void stop();

    // Both are for the network thread only. Neither ever blocks.
    [[nodiscard]] bool try_submit(const Command& command) noexcept {
        return commands_.try_push(command);
    }
    [[nodiscard]] bool try_take(Event& event) noexcept { return events_.try_pop(event); }

    // How many commands the engine thread has finished: processed, with all
    // their events queued. Once this equals the number of commands submitted
    // and the event queue has been emptied, nothing is left in the pipeline.
    [[nodiscard]] std::uint64_t processed() const noexcept {
        return processed_.load(std::memory_order_acquire);
    }

private:
    void run(const std::stop_token& stop);
    // Queues one event, waiting for room if necessary. Returns false if the
    // thread was asked to stop while waiting.
    bool emit(const Event& event, const std::stop_token& stop);

    SpscQueue<Command> commands_;
    SpscQueue<Event> events_;
    EngineWait wait_;
    MatchingEngine engine_;
    // Written by the engine thread after each command. On a cache line of its
    // own, away from what the network thread writes.
    alignas(64) std::atomic<std::uint64_t> processed_{0};
    std::jthread thread_;
};

} // namespace exchange::server
