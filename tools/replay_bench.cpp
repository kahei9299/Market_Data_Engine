// Benchmark: replay, sequential order book, and pipeline throughput/latency.
// Usage: replay_bench <jsonl_file>

#include "depth_update.hpp"
#include "order_book.hpp"
#include "pipeline.hpp"
#include "replay.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

struct LatencyStats {
    double mean_ns;
    double p50_ns;
    double p95_ns;
    double p99_ns;
    double max_ns;
};

static LatencyStats compute_latency(std::vector<int64_t>& samples) {
    if (samples.empty())
        return {};
    std::sort(samples.begin(), samples.end());
    auto n = samples.size();
    double sum = 0;
    for (auto s : samples) sum += s;
    return LatencyStats{
        sum / static_cast<double>(n),
        static_cast<double>(samples[n / 2]),
        static_cast<double>(samples[static_cast<std::size_t>(n * 0.95)]),
        static_cast<double>(samples[static_cast<std::size_t>(n * 0.99)]),
        static_cast<double>(samples.back()),
    };
}

static void print_latency(char const* name, LatencyStats const& s,
                           uint64_t count, ns wall) {
    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall);
    double throughput = count * 1e9 / wall.count();
    std::cout << name << ":\n"
              << "  messages:   " << count << '\n'
              << "  wall time:  " << wall_ms.count() << " ms\n"
              << "  throughput: " << std::fixed << std::setprecision(0)
              << throughput << " msg/s\n"
              << "  latency:\n"
              << "    mean: " << std::setprecision(0) << s.mean_ns << " ns\n"
              << "    p50:  " << s.p50_ns << " ns\n"
              << "    p95:  " << s.p95_ns << " ns\n"
              << "    p99:  " << s.p99_ns << " ns\n"
              << "    max:  " << s.max_ns << " ns\n";
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: replay_bench <jsonl_file>\n";
        return EXIT_FAILURE;
    }

    // Load and parse.
    auto raw = load_messages(argv[1]);
    if (raw.empty()) {
        std::cerr << "no messages loaded\n";
        return EXIT_FAILURE;
    }

    ReplayStats parse_stats;
    auto updates = parse_messages(raw, parse_stats);
    std::cout << "loaded " << raw.size() << " raw, parsed "
              << updates.size() << " updates ("
              << parse_stats.parse_errors << " errors)\n\n";

    if (updates.empty()) {
        std::cerr << "no valid updates\n";
        return EXIT_FAILURE;
    }

    // --- Benchmark 1: Parse throughput ---
    {
        auto start = Clock::now();
        ReplayStats s;
        auto parsed = parse_messages(raw, s);
        auto elapsed = Clock::now() - start;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        double throughput = raw.size() * 1e9 / elapsed.count();
        std::cout << "parse:\n"
                  << "  messages:   " << raw.size() << '\n'
                  << "  wall time:  " << ms.count() << " ms\n"
                  << "  throughput: " << std::fixed << std::setprecision(0)
                  << throughput << " msg/s\n\n";
    }

    // --- Benchmark 2: Sequential replay with per-message latency ---
    {
        std::vector<int64_t> latencies;
        latencies.reserve(updates.size());

        OrderBook book;
        Snapshot init{0, {}, {}};
        book.apply_snapshot(init);

        auto wall_start = Clock::now();
        for (auto const& u : updates) {
            auto t0 = Clock::now();
            book.apply_update(u);
            auto t1 = Clock::now();
            latencies.push_back(
                std::chrono::duration_cast<ns>(t1 - t0).count());
        }
        auto wall = Clock::now() - wall_start;

        auto lat = compute_latency(latencies);
        print_latency("sequential replay", lat, updates.size(), wall);

        auto bb = book.best_bid();
        auto ba = book.best_ask();
        std::cout << "  final: update_id=" << book.update_id()
                  << " bids=" << book.bid_count()
                  << " asks=" << book.ask_count();
        if (bb)
            std::cout << " best_bid=" << format_scaled(bb->first);
        if (ba)
            std::cout << " best_ask=" << format_scaled(ba->first);
        std::cout << "\n\n";
    }

    // --- Benchmark 3: Pipeline replay ---
    for (std::size_t qcap : {256, 1024, 4096, 16384}) {
        auto wall_start = Clock::now();
        auto result = replay_pipeline(updates, 0, qcap);
        auto wall = Clock::now() - wall_start;

        auto wall_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(wall);
        double throughput = updates.size() * 1e9 / wall.count();

        std::cout << "pipeline (queue=" << qcap << "):\n"
                  << "  messages:   " << updates.size() << '\n'
                  << "  wall time:  " << wall_ms.count() << " ms\n"
                  << "  throughput: " << std::fixed << std::setprecision(0)
                  << throughput << " msg/s\n"
                  << "  applied:    " << result.stats.replay.messages_applied
                  << '\n'
                  << "  stale:      " << result.stats.replay.messages_stale
                  << '\n'
                  << "  gaps:       " << result.stats.replay.messages_gap
                  << '\n'
                  << "  full spins: " << result.stats.queue_full_spins
                  << '\n'
                  << "  empty spins:" << result.stats.queue_empty_spins
                  << '\n'
                  << "  producer:   "
                  << std::chrono::duration_cast<
                         std::chrono::milliseconds>(
                         result.stats.producer_time).count()
                  << " ms\n"
                  << "  consumer:   "
                  << std::chrono::duration_cast<
                         std::chrono::milliseconds>(
                         result.stats.consumer_time).count()
                  << " ms\n"
                  << "  final: update_id=" << result.book.update_id()
                  << " bids=" << result.book.bid_count()
                  << " asks=" << result.book.ask_count()
                  << "\n\n";
    }

    return EXIT_SUCCESS;
}
