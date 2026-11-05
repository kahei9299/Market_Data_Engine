// Benchmark: parse, order-book application, and SPSC transfer throughput.
// Usage: replay_bench <jsonl_file>

#include "depth_update.hpp"
#include "order_book.hpp"
#include "pipeline.hpp"
#include "replay.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

static constexpr int kWarmupRuns = 2;
static constexpr int kMeasuredRuns = 5;

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

static ns median_duration(std::vector<ns>& durations) {
    std::sort(durations.begin(), durations.end());
    return durations[durations.size() / 2];
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: replay_bench <jsonl_file>\n";
        return EXIT_FAILURE;
    }

    auto raw = load_messages(argv[1]);
    if (raw.empty()) {
        std::cerr << "no messages loaded\n";
        return EXIT_FAILURE;
    }

    ReplayStats parse_stats;
    auto updates = parse_messages(raw, parse_stats);
    std::cout << "loaded " << raw.size() << " raw, parsed "
              << updates.size() << " updates ("
              << parse_stats.parse_errors << " errors)\n"
              << "warmup=" << kWarmupRuns << " measured=" << kMeasuredRuns
              << " (median reported)\n\n";

    if (updates.empty()) {
        std::cerr << "no valid updates\n";
        return EXIT_FAILURE;
    }

    // --- Benchmark 1: Parse-only throughput ---
    {
        for (int i = 0; i < kWarmupRuns; ++i) {
            ReplayStats s;
            parse_messages(raw, s);
        }
        std::vector<ns> runs;
        for (int i = 0; i < kMeasuredRuns; ++i) {
            auto start = Clock::now();
            ReplayStats s;
            parse_messages(raw, s);
            runs.push_back(Clock::now() - start);
        }
        auto wall = median_duration(runs);
        double throughput = raw.size() * 1e9 / wall.count();
        std::cout << "parse-only throughput:\n"
                  << "  messages:   " << raw.size() << '\n'
                  << "  wall time:  "
                  << std::chrono::duration_cast<
                         std::chrono::milliseconds>(wall).count()
                  << " ms (median of " << kMeasuredRuns << ")\n"
                  << "  throughput: " << std::fixed << std::setprecision(0)
                  << throughput << " msg/s\n\n";
    }

    // --- Benchmark 2: Sequential order-book application ---
    {
        // Warm up.
        for (int i = 0; i < kWarmupRuns; ++i)
            replay_sequential(updates, 0);

        std::vector<ns> runs;
        LatencyStats best_lat{};
        OrderBook final_book;

        for (int i = 0; i < kMeasuredRuns; ++i) {
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
            runs.push_back(Clock::now() - wall_start);

            if (i == kMeasuredRuns / 2) {
                best_lat = compute_latency(latencies);
                final_book = std::move(book);
            }
        }

        auto wall = median_duration(runs);
        double throughput = updates.size() * 1e9 / wall.count();
        std::cout << "sequential order-book application:\n"
                  << "  messages:   " << updates.size() << '\n'
                  << "  wall time:  "
                  << std::chrono::duration_cast<
                         std::chrono::milliseconds>(wall).count()
                  << " ms (median of " << kMeasuredRuns << ")\n"
                  << "  throughput: " << std::fixed << std::setprecision(0)
                  << throughput << " msg/s\n"
                  << "  per-update latency (median run):\n"
                  << "    mean: " << best_lat.mean_ns << " ns\n"
                  << "    p50:  " << best_lat.p50_ns << " ns\n"
                  << "    p95:  " << best_lat.p95_ns << " ns\n"
                  << "    p99:  " << best_lat.p99_ns << " ns\n"
                  << "    max:  " << best_lat.max_ns << " ns\n";

        auto bb = final_book.best_bid();
        auto ba = final_book.best_ask();
        std::cout << "  final: update_id=" << final_book.update_id()
                  << " bids=" << final_book.bid_count()
                  << " asks=" << final_book.ask_count();
        if (bb) std::cout << " best_bid=" << format_scaled(bb->first);
        if (ba) std::cout << " best_ask=" << format_scaled(ba->first);
        std::cout << "\n\n";
    }

    // --- Benchmark 3: SPSC transfer + order-book application ---
    for (std::size_t qcap : {256, 1024, 4096, 16384}) {
        for (int i = 0; i < kWarmupRuns; ++i)
            replay_pipeline(updates, 0, qcap);

        std::vector<ns> runs;
        PipelineResult best_result{};

        for (int i = 0; i < kMeasuredRuns; ++i) {
            auto start = Clock::now();
            auto result = replay_pipeline(updates, 0, qcap);
            runs.push_back(Clock::now() - start);
            if (i == kMeasuredRuns / 2)
                best_result = std::move(result);
        }

        auto wall = median_duration(runs);
        double throughput = updates.size() * 1e9 / wall.count();

        std::cout << "SPSC transfer + order-book application (queue="
                  << qcap << "):\n"
                  << "  messages:   " << updates.size() << '\n'
                  << "  wall time:  "
                  << std::chrono::duration_cast<
                         std::chrono::milliseconds>(wall).count()
                  << " ms (median of " << kMeasuredRuns << ")\n"
                  << "  throughput: " << std::fixed << std::setprecision(0)
                  << throughput << " msg/s\n"
                  << "  applied:    "
                  << best_result.stats.replay.messages_applied << '\n'
                  << "  complete:   "
                  << (best_result.stats.replay.complete ? "yes" : "no") << '\n'
                  << "  full spins: "
                  << best_result.stats.queue_full_spins << '\n'
                  << "  empty spins:"
                  << best_result.stats.queue_empty_spins << '\n'
                  << "  final: update_id=" << best_result.book.update_id()
                  << " bids=" << best_result.book.bid_count()
                  << " asks=" << best_result.book.ask_count()
                  << "\n\n";
    }

    return EXIT_SUCCESS;
}
