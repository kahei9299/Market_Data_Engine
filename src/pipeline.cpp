#include "pipeline.hpp"

PipelineResult replay_pipeline(std::vector<DepthUpdate> const& updates,
                               uint64_t initial_update_id,
                               std::size_t queue_capacity) {
    if (queue_capacity == 0) queue_capacity = 1;

    SpscQueue<DepthUpdate> queue(queue_capacity);
    std::atomic<bool> producer_done{false};
    std::atomic<bool> consumer_gap{false};

    PipelineStats stats;
    OrderBook book;
    {
        Snapshot init{initial_update_id, {}, {}};
        book.apply_snapshot(init);
    }

    std::thread consumer([&]() {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            auto item = queue.try_pop();
            if (item) {
                auto status = book.apply_update(*item);
                switch (status) {
                case UpdateResult::Applied:
                    stats.replay.messages_applied++;
                    break;
                case UpdateResult::Stale:
                    stats.replay.messages_stale++;
                    break;
                case UpdateResult::Gap:
                    stats.replay.messages_gap++;
                    consumer_gap.store(true, std::memory_order_release);
                    stats.consumer_time =
                        std::chrono::steady_clock::now() - start;
                    return;
                }
            } else if (producer_done.load(std::memory_order_acquire)) {
                while (auto remaining = queue.try_pop()) {
                    auto status = book.apply_update(*remaining);
                    if (status == UpdateResult::Applied)
                        stats.replay.messages_applied++;
                    else if (status == UpdateResult::Stale)
                        stats.replay.messages_stale++;
                    else {
                        stats.replay.messages_gap++;
                        stats.consumer_time =
                            std::chrono::steady_clock::now() - start;
                        return;
                    }
                }
                break;
            } else {
                stats.queue_empty_spins++;
            }
        }
        stats.replay.complete = true;
        stats.consumer_time = std::chrono::steady_clock::now() - start;
    });

    auto pstart = std::chrono::steady_clock::now();
    for (auto const& update : updates) {
        if (consumer_gap.load(std::memory_order_acquire))
            break;
        while (!queue.try_push(update)) {
            if (consumer_gap.load(std::memory_order_acquire))
                break;
            stats.queue_full_spins++;
        }
    }
    producer_done.store(true, std::memory_order_release);
    stats.producer_time = std::chrono::steady_clock::now() - pstart;

    consumer.join();

    stats.replay.messages_parsed = updates.size();
    return PipelineResult{std::move(book), std::move(stats)};
}
