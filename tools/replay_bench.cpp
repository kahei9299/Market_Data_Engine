// Benchmark: parse, order-book application, and SPSC transfer throughput.
// Usage: replay_bench <jsonl_file>
//
// Methodology:
// - Warm-up runs are discarded to stabilize caches and branch predictors.
// - Multiple measured runs are performed; the median wall time is reported.
// - Per-update latency percentiles are taken from the median-duration run.
// - All measurements use std::chrono::steady_clock (monotonic).

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

static std::size_t median_index(std::vector<ns> const& durations) {
    auto sorted_indices = std::vector<std::size_t>(durations.size());
    std::iota(sorted_indices.begin(), sorted_indices.end(), 0);
    std::sort(sorted_indices.begin(), sorted_indices.end(),
              [&](auto a, auto b) { return durations[a] < durations[b]; });
    return sorted_indices[sorted_indices.size() / 2];
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
    std::cout << "=== Market Data Engine Benchmark ===\n"
              << "messages: " << updates.size() << " ("
              << parse_stats.parse_errors << " parse errors)\n"
              << "warmup: " << kWarmupRuns
              << ", measured: " << kMeasuredRuns << " (median reported)\n"
#ifdef __APPLE__
              << "platform: macOS, "
#else
              << "platform: Linux, "
#endif
#ifdef __clang__
              << "clang " << __clang_major__ << "." << __clang_minor__
#elif defined(__GNUC__)
              << "gcc " << __GNUC__ << "." << __GNUC_MINOR__
#endif
#ifdef NDEBUG
              << ", Release\n\n";
#else
              << ", Debug\n\n";
#endif

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
        auto mid = median_index(runs);
        auto wall = runs[mid];
        double throughput = raw.size() * 1e9 / wall.count();
        std::cout << "parse-only throughput:\n"
                  << "  messages:   " << raw.size() << '\n'
                  << "  wall time:  "
                  << std::chrono::duration_cast<
                         std::chrono::milliseconds>(wall).count()
                  << " ms\n"
                  << "  throughput: " << std::fixed << std::setprecision(0)
                  << throughput << " msg/s\n\n";
    }

    // --- Benchmark 2: Sequential order-book application ---
    {
        for (int i = 0; i < kWarmupRuns; ++i)
            replay_sequential(updates, 0);

        struct RunData {
            ns duration;
            std::vector<int64_t> latencies;
            OrderBook book;
        };
        std::vector<RunData> runs(kMeasuredRuns);

        for (int i = 0; i < kMeasuredRuns; ++i) {
            runs[i].latencies.reserve(updates.size());
            OrderBook book;
            Snapshot init{0, {}, {}};
            book.apply_snapshot(init);

            auto wall_start = Clock::now();
            for (auto const& u : updates) {
                auto t0 = Clock::now();
                book.apply_update(u);
                auto t1 = Clock::now();
                runs[i].latencies.push_back(
                    std::chrono::duration_cast<ns>(t1 - t0).count());
            }
            runs[i].duration = Clock::now() - wall_start;
            runs[i].book = std::move(book);
        }

        // Find the median-duration run.
        std::vector<ns> durations;
        for (auto& r : runs) durations.push_back(r.duration);
        auto mid = median_index(durations);

        auto& best = runs[mid];
        auto lat = compute_latency(best.latencies);
        double throughput = updates.size() * 1e9 / best.duration.count();

        std::cout << "sequential order-book application:\n"
                  << "  messages:   " << updates.size() << '\n'
                  << "  wall time:  "
                  << std::chrono::duration_cast<
                         std::chrono::milliseconds>(best.duration).count()
                  << " ms\n"
                  << "  throughput: " << std::fixed << std::setprecision(0)
                  << throughput << " msg/s\n"
                  << "  per-update latency (from median-duration run):\n"
                  << "    mean: " << lat.mean_ns << " ns\n"
                  << "    p50:  " << lat.p50_ns << " ns\n"
                  << "    p95:  " << lat.p95_ns << " ns\n"
                  << "    p99:  " << lat.p99_ns << " ns\n"
                  << "    max:  " << lat.max_ns << " ns\n";

        auto bb = best.book.best_bid();
        auto ba = best.book.best_ask();
        std::cout << "  final: update_id=" << best.book.update_id()
                  << " bids=" << best.book.bid_count()
                  << " asks=" << best.book.ask_count();
        if (bb) std::cout << " best_bid=" << format_scaled(bb->first);
        if (ba) std::cout << " best_ask=" << format_scaled(ba->first);
        std::cout << "\n\n";
    }

    // --- Benchmark 3: SPSC transfer + order-book application ---
    for (std::size_t qcap : {1024, 4096, 16384}) {
        for (int i = 0; i < kWarmupRuns; ++i)
            replay_pipeline(updates, 0, qcap);

        std::vector<ns> durations;
        PipelineResult median_result{};

        struct PipeRun {
            ns duration;
            PipelineResult result;
        };
        std::vector<PipeRun> runs(kMeasuredRuns);

        for (int i = 0; i < kMeasuredRuns; ++i) {
            auto start = Clock::now();
            runs[i].result = replay_pipeline(updates, 0, qcap);
            runs[i].duration = Clock::now() - start;
            durations.push_back(runs[i].duration);
        }

        auto mid = median_index(durations);
        auto& best = runs[mid];
        double throughput = updates.size() * 1e9 / best.duration.count();

        std::cout << "SPSC transfer + order-book application (queue="
                  << qcap << "):\n"
                  << "  messages:   " << updates.size() << '\n'
                  << "  wall time:  "
                  << std::chrono::duration_cast<
                         std::chrono::milliseconds>(best.duration).count()
                  << " ms\n"
                  << "  throughput: " << std::fixed << std::setprecision(0)
                  << throughput << " msg/s\n"
                  << "  applied:    "
                  << best.result.stats.replay.messages_applied << '\n'
                  << "  complete:   "
                  << (best.result.stats.replay.complete ? "yes" : "no") << '\n'
                  << "  full spins: "
                  << best.result.stats.queue_full_spins << '\n'
                  << "  empty spins:"
                  << best.result.stats.queue_empty_spins << '\n'
                  << "  final: update_id=" << best.result.book.update_id()
                  << " bids=" << best.result.book.bid_count()
                  << " asks=" << best.result.book.ask_count()
                  << "\n\n";
    }

    std::cout << "=== Measurement notes ===\n"
              << "- Latency measured per apply_update() call via steady_clock\n"
              << "- Clock overhead (~20-40ns) included in each measurement\n"
              << "- Pipeline throughput limited by std::map operations in consumer\n"
              << "- Synthetic data has ~10k unique price levels (wider than typical)\n";

    return EXIT_SUCCESS;
}
