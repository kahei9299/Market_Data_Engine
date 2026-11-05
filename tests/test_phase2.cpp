#define BOOST_TEST_MODULE phase2_tests
#include <boost/test/unit_test.hpp>

#include "depth_update.hpp"
#include "order_book.hpp"
#include "pipeline.hpp"
#include "replay.hpp"
#include "spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// --- SPSC queue tests ---

BOOST_AUTO_TEST_SUITE(spsc_queue_tests)

BOOST_AUTO_TEST_CASE(capacity_rounds_to_power_of_two) {
    SpscQueue<int> q(100);
    BOOST_TEST(q.capacity() == 128u);

    SpscQueue<int> q2(128);
    BOOST_TEST(q2.capacity() == 128u);

    SpscQueue<int> q3(1);
    BOOST_TEST(q3.capacity() == 1u);
}

BOOST_AUTO_TEST_CASE(push_pop_single) {
    SpscQueue<int> q(4);
    BOOST_TEST(q.try_push(42));
    auto v = q.try_pop();
    BOOST_REQUIRE(v.has_value());
    BOOST_TEST(*v == 42);
}

BOOST_AUTO_TEST_CASE(pop_empty_returns_nullopt) {
    SpscQueue<int> q(4);
    BOOST_TEST(!q.try_pop().has_value());
}

BOOST_AUTO_TEST_CASE(push_full_returns_false) {
    SpscQueue<int> q(2);
    BOOST_TEST(q.try_push(1));
    BOOST_TEST(q.try_push(2));
    BOOST_TEST(!q.try_push(3));
}

BOOST_AUTO_TEST_CASE(fifo_order) {
    SpscQueue<int> q(8);
    for (int i = 0; i < 5; ++i)
        BOOST_REQUIRE(q.try_push(i));
    for (int i = 0; i < 5; ++i) {
        auto v = q.try_pop();
        BOOST_REQUIRE(v.has_value());
        BOOST_TEST(*v == i);
    }
}

BOOST_AUTO_TEST_CASE(wraparound) {
    SpscQueue<int> q(4);
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 4; ++i)
            BOOST_REQUIRE(q.try_push(round * 10 + i));
        for (int i = 0; i < 4; ++i) {
            auto v = q.try_pop();
            BOOST_REQUIRE(v.has_value());
            BOOST_TEST(*v == round * 10 + i);
        }
    }
}

BOOST_AUTO_TEST_CASE(capacity_one) {
    SpscQueue<int> q(1);
    BOOST_TEST(q.capacity() == 1u);
    BOOST_TEST(q.try_push(99));
    BOOST_TEST(!q.try_push(100));
    auto v = q.try_pop();
    BOOST_REQUIRE(v.has_value());
    BOOST_TEST(*v == 99);
    BOOST_TEST(!q.try_pop().has_value());
}

BOOST_AUTO_TEST_CASE(large_capacity) {
    SpscQueue<int> q(1 << 16); // 65536
    BOOST_TEST(q.capacity() == 65536u);
    for (int i = 0; i < 1000; ++i)
        BOOST_REQUIRE(q.try_push(i));
    for (int i = 0; i < 1000; ++i) {
        auto v = q.try_pop();
        BOOST_REQUIRE(v.has_value());
        BOOST_TEST(*v == i);
    }
}

BOOST_AUTO_TEST_CASE(concurrent_stress) {
    SpscQueue<uint64_t> q(1024);
    constexpr uint64_t N = 100000;
    std::atomic<bool> done{false};
    uint64_t sum_produced = 0;
    uint64_t sum_consumed = 0;
    uint64_t count_consumed = 0;

    std::thread producer([&]() {
        for (uint64_t i = 1; i <= N; ++i) {
            while (!q.try_push(i)) {}
            sum_produced += i;
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        while (true) {
            auto v = q.try_pop();
            if (v) {
                sum_consumed += *v;
                count_consumed++;
            } else if (done.load(std::memory_order_acquire)) {
                while (auto rem = q.try_pop()) {
                    sum_consumed += *rem;
                    count_consumed++;
                }
                break;
            }
        }
    });

    producer.join();
    consumer.join();

    BOOST_TEST(count_consumed == N);
    BOOST_TEST(sum_consumed == sum_produced);
}

BOOST_AUTO_TEST_SUITE_END()

// --- Replay tests ---

BOOST_AUTO_TEST_SUITE(replay_tests)

static DepthUpdate make_update(uint64_t U, uint64_t u,
                               std::vector<PriceLevel> bids = {},
                               std::vector<PriceLevel> asks = {}) {
    return DepthUpdate{"BTCUSDT", 0, U, u, std::move(bids), std::move(asks)};
}

BOOST_AUTO_TEST_CASE(sequential_replay_applies_updates) {
    std::vector<DepthUpdate> updates = {
        make_update(1, 1, {{100, 50}}, {{200, 30}}),
        make_update(2, 2, {{100, 0}}, {}),
        make_update(3, 3, {{150, 80}}, {{200, 0}}),
    };

    auto result = replay_sequential(updates, 0);

    BOOST_TEST(result.stats.messages_applied == 3u);
    BOOST_TEST(result.stats.messages_stale == 0u);
    BOOST_TEST(result.stats.messages_gap == 0u);
    BOOST_TEST(result.stats.complete);
    BOOST_TEST(result.book.update_id() == 3u);
    BOOST_TEST(result.book.bid_count() == 1u);
    BOOST_TEST(result.book.ask_count() == 0u);
    BOOST_TEST(result.book.best_bid()->first == 150);
}

BOOST_AUTO_TEST_CASE(replay_detects_stale) {
    std::vector<DepthUpdate> updates = {
        make_update(1, 2, {{100, 50}}, {}),
        make_update(1, 1, {{100, 99}}, {}), // stale
        make_update(3, 3, {{200, 60}}, {}),
    };

    auto result = replay_sequential(updates, 0);
    BOOST_TEST(result.stats.messages_applied == 2u);
    BOOST_TEST(result.stats.messages_stale == 1u);
    BOOST_TEST(result.stats.complete);
    BOOST_TEST(result.book.best_bid()->second == 60);
}

BOOST_AUTO_TEST_CASE(replay_stops_on_gap) {
    std::vector<DepthUpdate> updates = {
        make_update(1, 1, {{100, 50}}, {}),
        make_update(5, 5, {{100, 99}}, {}), // gap
        make_update(6, 6, {{100, 77}}, {}), // never reached
    };

    auto result = replay_sequential(updates, 0);
    BOOST_TEST(result.stats.messages_applied == 1u);
    BOOST_TEST(result.stats.messages_gap == 1u);
    BOOST_TEST(!result.stats.complete);
    BOOST_TEST(result.book.best_bid()->second == 50);
}

BOOST_AUTO_TEST_CASE(load_and_parse_from_file) {
    auto tmpdir = std::string(std::getenv("TMPDIR") ?: "/tmp");
    auto path = tmpdir + "/test_replay.jsonl";
    {
        std::ofstream out(path);
        out << R"({"e":"depthUpdate","E":100,"s":"BTCUSDT","U":1,"u":1,"b":[["100.0","1.0"]],"a":[]})" << '\n';
        out << R"({"e":"depthUpdate","E":101,"s":"BTCUSDT","U":2,"u":2,"b":[],"a":[["200.0","2.0"]]})" << '\n';
        out << "bad json\n";
    }

    auto raw = load_messages(path);
    BOOST_TEST(raw.size() == 3u);

    ReplayStats stats;
    auto updates = parse_messages(raw, stats);
    BOOST_TEST(updates.size() == 2u);
    BOOST_TEST(stats.messages_parsed == 2u);
    BOOST_TEST(stats.parse_errors == 1u);

    auto result = replay_sequential(updates, 0);
    BOOST_TEST(result.stats.complete);
    BOOST_TEST(result.book.update_id() == 2u);

    std::remove(path.c_str());
}

BOOST_AUTO_TEST_CASE(empty_file_handled) {
    auto tmpdir = std::string(std::getenv("TMPDIR") ?: "/tmp");
    auto path = tmpdir + "/test_empty.jsonl";
    { std::ofstream out(path); }

    auto raw = load_messages(path);
    BOOST_TEST(raw.empty());

    std::remove(path.c_str());
}

BOOST_AUTO_TEST_CASE(complete_replay_no_gaps) {
    std::vector<DepthUpdate> updates;
    for (uint64_t i = 1; i <= 100; ++i)
        updates.push_back(make_update(i, i, {{100, static_cast<int64_t>(i)}}, {}));

    auto result = replay_sequential(updates, 0);
    BOOST_TEST(result.stats.complete);
    BOOST_TEST(result.stats.messages_applied == 100u);
}

BOOST_AUTO_TEST_SUITE_END()

// --- Pipeline tests ---

BOOST_AUTO_TEST_SUITE(pipeline_tests)

static DepthUpdate make_update_p(uint64_t U, uint64_t u,
                                 std::vector<PriceLevel> bids = {},
                                 std::vector<PriceLevel> asks = {}) {
    return DepthUpdate{"BTCUSDT", 0, U, u, std::move(bids), std::move(asks)};
}

// Compare full bid/ask contents between two OrderBooks.
static void assert_books_equal(OrderBook const& a, OrderBook const& b) {
    BOOST_TEST(a.update_id() == b.update_id());
    BOOST_TEST(a.bid_count() == b.bid_count());
    BOOST_TEST(a.ask_count() == b.ask_count());
    BOOST_TEST((a.bids() == b.bids()));
    BOOST_TEST((a.asks() == b.asks()));
}

BOOST_AUTO_TEST_CASE(pipeline_matches_sequential_full_comparison) {
    std::vector<DepthUpdate> updates;
    for (uint64_t i = 1; i <= 1000; ++i) {
        int64_t price = static_cast<int64_t>(70000 + (i % 100)) * 100000000LL;
        int64_t qty = static_cast<int64_t>(i % 50) * 100000000LL;
        updates.push_back(make_update_p(i, i, {{price, qty}}, {}));
    }

    auto seq = replay_sequential(updates, 0);
    auto pip = replay_pipeline(updates, 0, 256);

    BOOST_TEST(seq.stats.complete);
    BOOST_TEST(pip.stats.replay.complete);
    assert_books_equal(seq.book, pip.book);
}

BOOST_AUTO_TEST_CASE(pipeline_handles_capacity_one) {
    std::vector<DepthUpdate> updates;
    for (uint64_t i = 1; i <= 50; ++i)
        updates.push_back(make_update_p(i, i, {{100, 50}}, {}));

    auto result = replay_pipeline(updates, 0, 1);
    BOOST_TEST(result.stats.replay.messages_applied == 50u);
    BOOST_TEST(result.stats.replay.complete);
}

BOOST_AUTO_TEST_CASE(pipeline_handles_zero_capacity) {
    // Zero capacity is clamped to 1 internally.
    std::vector<DepthUpdate> updates;
    for (uint64_t i = 1; i <= 10; ++i)
        updates.push_back(make_update_p(i, i, {{100, 50}}, {}));

    auto result = replay_pipeline(updates, 0, 0);
    BOOST_TEST(result.stats.replay.messages_applied == 10u);
    BOOST_TEST(result.stats.replay.complete);
}

BOOST_AUTO_TEST_CASE(pipeline_stops_on_gap) {
    std::vector<DepthUpdate> updates = {
        make_update_p(1, 1, {{100, 50}}, {}),
        make_update_p(5, 5, {{100, 99}}, {}), // gap
        make_update_p(6, 6, {{100, 77}}, {}), // should not be applied
    };

    auto result = replay_pipeline(updates, 0, 64);
    BOOST_TEST(result.stats.replay.messages_gap == 1u);
    BOOST_TEST(!result.stats.replay.complete);
    BOOST_TEST(result.book.best_bid()->second == 50);
}

BOOST_AUTO_TEST_CASE(pipeline_stress_full_comparison) {
    std::vector<DepthUpdate> updates;
    for (uint64_t i = 1; i <= 50000; ++i) {
        int64_t price = static_cast<int64_t>(70000 + (i % 200)) * 100000000LL;
        int64_t qty = static_cast<int64_t>(i % 100) * 100000000LL;
        updates.push_back(make_update_p(i, i, {{price, qty}},
                                        {{price + 100000000LL, qty}}));
    }

    auto seq = replay_sequential(updates, 0);
    auto pip = replay_pipeline(updates, 0, 4096);

    BOOST_TEST(seq.stats.complete);
    BOOST_TEST(pip.stats.replay.complete);
    assert_books_equal(seq.book, pip.book);
}

BOOST_AUTO_TEST_SUITE_END()
