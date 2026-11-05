#pragma once

#include "depth_update.hpp"
#include "order_book.hpp"
#include "replay.hpp"
#include "spsc_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

struct PipelineStats {
    ReplayStats replay;
    uint64_t queue_full_spins = 0;
    uint64_t queue_empty_spins = 0;
    std::chrono::nanoseconds producer_time{0};
    std::chrono::nanoseconds consumer_time{0};
};

struct PipelineResult {
    OrderBook book;
    PipelineStats stats;
};

// Replay parsed updates through a producer-consumer pipeline.
// Producer: pushes DepthUpdates into the SPSC queue.
// Consumer: pops and applies to OrderBook.
// Both threads spin on backpressure/empty (bounded busy-wait).
PipelineResult replay_pipeline(std::vector<DepthUpdate> const& updates,
                               uint64_t initial_update_id = 0,
                               std::size_t queue_capacity = 4096);
