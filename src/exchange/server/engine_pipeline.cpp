#include "exchange/server/engine_pipeline.hpp"

#include "exchange/queue/cpu_relax.hpp"

#include <chrono>
#include <vector>

namespace exchange::server {

namespace {

// Backoff strategy: how long to spin before starting to sleep, and for how long.
constexpr std::size_t spins_before_sleeping = 20'000;
constexpr std::chrono::microseconds nap(100);

} // namespace

EnginePipeline::EnginePipeline(const PipelineConfig& config)
    : commands_(config.command_queue_capacity), events_(config.event_queue_capacity),
      wait_(config.wait) {}

EnginePipeline::~EnginePipeline() {
    stop();
}

void EnginePipeline::start() {
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void EnginePipeline::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

bool EnginePipeline::emit(const Event& event, const std::stop_token& stop) {
    while (!events_.try_push(event)) {
        // The event queue is full: wait for the network thread to make room.
        // It is awake, because the command being processed is not finished,
        // and it takes events on every turn of its loop. It never waits for
        // this thread in return, so the two cannot block each other.
        if (stop.stop_requested()) {
            return false;
        }
        cpu_relax();
    }
    return true;
}

void EnginePipeline::run(const std::stop_token& stop) {
    std::vector<Event> events;
    events.reserve(4096);
    Command command;
    std::size_t idle_polls = 0;
    std::uint64_t processed = 0;

    while (!stop.stop_requested()) {
        if (!commands_.try_pop(command)) {
            if (wait_ == EngineWait::Backoff && ++idle_polls > spins_before_sleeping) {
                std::this_thread::sleep_for(nap);
            } else {
                cpu_relax();
            }
            continue;
        }
        idle_polls = 0;

        events.clear();
        engine_.process(command, events);

        for (const Event& event : events) {
            if (!emit(event, stop)) {
                return;
            }
        }
        // Counted only now that every event of the command is queued: the
        // network thread relies on this order to know when nothing is left.
        processed_.store(++processed, std::memory_order_release);
    }
}

} // namespace exchange::server
