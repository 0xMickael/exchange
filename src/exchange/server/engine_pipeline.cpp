#include "exchange/server/engine_pipeline.hpp"

#include "exchange/queue/cpu_relax.hpp"

#include <unistd.h>

#include <chrono>
#include <vector>

namespace exchange::server {

namespace {

// After this many commands the network thread is notified even if more
// commands are waiting, so that events are not held back by a long burst.
constexpr std::size_t commands_per_notification = 64;

// Backoff strategy: how long to spin before starting to sleep, and for how long.
constexpr std::size_t spins_before_sleeping = 20'000;
constexpr std::chrono::microseconds nap(100);

} // namespace

EnginePipeline::EnginePipeline(const PipelineConfig& config, int notify_fd)
    : commands_(config.command_queue_capacity), events_(config.event_queue_capacity),
      wait_(config.wait), notify_fd_(notify_fd) {}

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

void EnginePipeline::notify() const noexcept {
    const std::uint64_t one = 1;
    const ssize_t written = ::write(notify_fd_, &one, sizeof(one));
    static_cast<void>(written);
}

bool EnginePipeline::emit(const Event& event, const std::stop_token& stop) {
    bool announced_full_queue = false;
    while (!events_.try_push(event)) {
        // The event queue is full. The network thread may be asleep, unaware
        // that events are waiting: wake it, then wait for it to make room. It
        // never waits for this thread in return, so the two cannot block each
        // other.
        if (!announced_full_queue) {
            notify();
            announced_full_queue = true;
        }
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
    // True when events have been queued since the last notification.
    bool events_unannounced = false;
    std::size_t commands_since_notification = 0;
    std::size_t idle_polls = 0;
    std::uint64_t processed = 0;

    while (!stop.stop_requested()) {
        if (!commands_.try_pop(command)) {
            // Nothing left to do: make sure the network thread knows about
            // every event produced so far, then wait for the next command.
            if (events_unannounced) {
                notify();
                events_unannounced = false;
                commands_since_notification = 0;
            }
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
            events_unannounced = true;
        }
        processed_.store(++processed, std::memory_order_release);

        if (events_unannounced && ++commands_since_notification >= commands_per_notification) {
            notify();
            events_unannounced = false;
            commands_since_notification = 0;
        }
    }
}

} // namespace exchange::server
