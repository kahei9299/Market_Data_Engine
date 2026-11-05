# Market Data Engine

A production-oriented C++20 market-data engine that ingests live Binance exchange data over WebSocket, reconstructs a synchronized local order book using REST snapshots, and provides deterministic offline replay with measured performance characteristics.

Built as a systems engineering project demonstrating: async networking, TLS security, protocol-correct exchange integration, lock-free concurrency, and reproducible benchmarking.

## Architecture

```
Live path                                    Offline path

Binance WebSocket API                        JSONL file
(stream.binance.com:9443)                    (recorded depth updates)
        │                                           │
        ▼                                           ▼
   Session (async TLS WebSocket)             load_messages()
        │                                           │
        ▼                                           ▼
   parse_depth_update()                      parse_messages()
   JSON → DepthUpdate struct                 JSON → DepthUpdate vector
   decimal strings → int64 (×10⁸)                   │
        │                                    ┌──────┴──────┐
        ▼                                    ▼              ▼
   SyncEngine                          Sequential      Pipeline
   ├─ State: Buffering → Snapshot      replay          (SPSC queue)
   │         Pending → Synchronized    ┌────────┐    ┌────────────┐
   ├─ HttpClient (REST snapshot)       │ Order  │    │ Producer → │
   ├─ Bounded event buffer             │ Book   │    │ SPSC queue │
   └─ Gap detection → resync           │(single)│    │ → Consumer │
        │                              └────────┘    │ → OrderBook│
        ▼                                            └────────────┘
   OrderBook
   ├─ std::map<price, qty> × 2 sides
   ├─ apply_update() → Applied|Stale|Gap
   └─ best_bid() / best_ask()
```

**Threading model:** Single-threaded `io_context` event loop for the live path. The pipeline uses two threads (producer + consumer) with a lock-free SPSC queue.

## Building

Requires macOS with Homebrew, CMake 3.20+, and a C++20 compiler.

```bash
# Install dependencies
brew install boost ninja

# Build (Release for benchmarks, Debug for development)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build

# Run tests
./build/tests          # M1/M2 tests (123 cases)
./build/tests_phase2   # Replay/concurrency tests (20 cases)

# Run live receiver (Ctrl-C to stop)
./build/market-data-engine

# Record live data for offline replay (Ctrl-C to stop)
MARKET_DATA_RAW=1 ./build/market-data-engine 2>/dev/null > data/live.jsonl
# Or with a time limit (requires GNU coreutils: brew install coreutils)
# gtimeout 60 env MARKET_DATA_RAW=1 ./build/market-data-engine 2>/dev/null > data/live.jsonl

# Generate synthetic test data
./build/generate_data 100000 data/depth_100k.jsonl

# Run benchmarks
./build/replay_bench data/depth_100k.jsonl
```

### Sanitizer builds

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ninja -C build-asan && ./build-asan/tests && ./build-asan/tests_phase2

# ThreadSanitizer (concurrency tests)
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
ninja -C build-tsan tests_phase2 && ./build-tsan/tests_phase2
```

## Project structure

```
src/
├── main.cpp              Entry point, io_context, signal handling
├── session.hpp/cpp       Async WebSocket session (TLS, read loop, SyncEngine integration)
├── sync_engine.hpp/cpp   Binance 7-step sync protocol (state machine, retry, gap detection)
├── http_client.hpp/cpp   Async HTTPS client (persistent connection, TLS, deadline timer)
├── order_book.hpp/cpp    OrderBook (std::map bid/ask sides, update procedure)
├── depth_update.hpp/cpp  DepthUpdate parser, shared parse helpers
├── snapshot.hpp/cpp      REST depth snapshot parser
├── replay.hpp/cpp        Deterministic offline replay engine
├── spsc_queue.hpp        Lock-free SPSC ring buffer (header-only)
├── pipeline.hpp/cpp      Producer-consumer pipeline using SPSC queue
└── util.hpp/cpp          Timestamp, scaled decimal parser/formatter
tools/
├── generate_data.cpp     Synthetic depth update generator
└── replay_bench.cpp      Benchmark harness (parse, replay, pipeline)
tests/
├── test_main.cpp         123 M1/M2 tests: parsing, order book, networking, sync
└── test_phase2.cpp       20 Phase 2 tests: SPSC queue, replay, pipeline correctness
docs/
├── DECISIONS.md          Engineering decision log
└── LEARNING.md           C++ and systems concepts journal
```

## Binance Order Book Synchronization

Implements the [official Binance Spot protocol](https://github.com/binance/binance-spot-api-docs/blob/master/web-socket-streams.md#how-to-manage-a-local-order-book-correctly):

1. Open WebSocket to `btcusdt@depth`, buffer incoming events
2. On first event, note `U` and request REST snapshot (`GET /api/v3/depth`)
3. If snapshot `lastUpdateId < first_U` → stale, retry with exponential backoff
4. Discard buffered events where `u ≤ lastUpdateId`
5. Apply snapshot to book (`update_id = lastUpdateId`)
6. Apply remaining buffered events, then live events
7. On sequence gap (`U > update_id + 1`) → full resynchronization

**State machine:** `Buffering → SnapshotPending → Synchronized`, with resync on any failure.

**Key invariants:**
- Prices as signed `int64_t` scaled by 10⁸ (exact decimal, no floating-point)
- Zero-quantity updates delete the price level
- Atomic parse failure — never a partial `DepthUpdate`
- Single-writer OrderBook (SyncEngine or consumer thread)

## Concurrency Design

The multithreaded pipeline is used for **offline replay benchmarking only**. The live WebSocket path remains single-threaded on one `io_context`.

### Lock-free SPSC Queue

The `SpscQueue<T>` is a single-producer single-consumer bounded ring buffer:

- Power-of-two capacity with bitmask indexing (no modulo). Requested capacity is rounded up to the next power of two. Extremely large capacities are not validated against `size_t` overflow.
- `alignas(64)` on head/tail atomics to prevent false sharing
- `memory_order_release` on publish, `memory_order_acquire` on consume
- `try_push()` / `try_pop()` — non-blocking, no allocation
- Backpressure: producer yields on full queue

### Pipeline

```
Producer thread          SPSC Queue          Consumer thread
(push DepthUpdates) ──→ [ring buffer] ──→ (apply to OrderBook)
```

- Consumer is the sole writer to the OrderBook
- Producer stops on consumer gap detection (atomic flag)
- Both stop-on-first-gap for deterministic replay
- `std::thread::yield()` on spin to reduce spin iterations (not profiled for CPU utilization)
- Verified: pipeline produces **identical** book state as sequential replay

### Why the pipeline doesn't improve throughput

The pipeline splits queuing from book application across two threads, but the consumer (order-book updates via `std::map`) is the bottleneck at ~9µs per update. The SPSC queue transfer cost (~10-50ns) is negligible. Adding a thread does not reduce the per-update `std::map` cost — the same work runs on a different core. The pipeline demonstrates correct lock-free producer-consumer design, not a throughput improvement for this workload. Throughput gains would require reducing the per-update cost (e.g., a flatter data structure) or parallelizing across independent books.

## Performance

**Environment:** Apple M4 Pro, macOS 15, Clang 21, `-O2`, 100k synthetic messages. All benchmarks use offline replay of pre-parsed data — not live exchange processing.

**Methodology:** 2 warm-up + 5 measured runs, median wall time reported. Per-update latency percentiles from the median-duration run. `steady_clock` for all measurements. Results reflect synthetic data performance, not verified real-market-data throughput.

| Metric | Value |
|---|---|
| **Parse-only throughput** | 801k msg/s |
| **Sequential order-book application** | 111k msg/s |
| Per-update mean latency | 8,972 ns |
| Per-update p50 latency | 8,708 ns |
| Per-update p95 latency | 11,208 ns |
| Per-update p99 latency | 13,625 ns |
| **SPSC + book application (q=4096)** | 109k msg/s |

**Bottleneck:** `std::map::operator[]` and `std::map::erase` at O(log n) with ~10k entries. Each update modifies 1-5 price levels, requiring ~13 key comparisons per operation plus node allocation.

**Limitations:**
- Synthetic data has ~10k unique prices (wider than a typical real book)
- `steady_clock` overhead (~20-40ns) included in per-message measurements
- Single machine, single run configuration

## Recording Live Market Data

```bash
# Record raw depth updates (Ctrl-C to stop)
MARKET_DATA_RAW=1 ./build/market-data-engine 2>/dev/null > data/live.jsonl

# Replay recorded data
./build/replay_bench data/live.jsonl
```

The replay engine strips timestamp prefixes from recorded data. **Important:** Recorded differential updates without a matching REST snapshot do not produce a verified synchronized order book. Offline replay starts from an empty book and applies updates sequentially — the book state converges toward the exchange's state as more price levels are touched, but levels that were never updated remain missing. This mode is useful for benchmarking parser and book-update performance, not for verifying order book accuracy.

## Testing

**143 tests total** (123 in `test_main.cpp` + 20 in `test_phase2.cpp`):

- **ASan + UBSan:** All 143 tests passed.
- **ThreadSanitizer:** All 20 Phase 2 tests passed (concurrency-relevant subset).

| Suite | Count | Coverage |
|---|---|---|
| Scaled decimal parsing | 20 | Precision, overflow, INT64_MAX boundary, negative zero |
| Depth update parsing | 24 | Valid/invalid JSON, field validation, atomic failure |
| Snapshot parsing | 10 | REST response, zero-qty filtering, malformed data |
| Order book operations | 21 | Insert/update/delete, gap/stale, crossed book, UINT64_MAX |
| TLS and networking | 1+ | Hostname verification, connection lifecycle |
| HttpClient | 9 | GET, reuse, timeout, cancel, TLS mismatch, stale recovery |
| SyncEngine | 11 | Sync protocol, retry, buffer overflow, generation isolation |
| Lifetime regression | 9 | Async lifetime, abort recovery, resource safety |
| SPSC queue | 9 | Capacity, FIFO, wraparound, concurrent stress |
| Replay | 6 | Sequential, gap stop, file I/O, completeness |
| Pipeline | 5 | Correctness vs sequential, gap stop, edge capacities |

Mock HTTPS server tests require local port binding (skipped in sandboxed CI).

## Dependencies

| Library | Purpose |
|---|---|
| [Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/) | WebSocket and HTTP protocol |
| [Boost.Asio](https://www.boost.org/doc/libs/release/libs/asio/) | Async I/O, TCP, DNS, signals, timers |
| [Boost.JSON](https://www.boost.org/doc/libs/release/libs/json/) | JSON parsing |
| [OpenSSL](https://www.openssl.org/) | TLS encryption, certificate verification |
| [Boost.Test](https://www.boost.org/doc/libs/release/libs/test/) | Unit testing |

No additional runtime dependencies. No custom allocators, no external benchmarking frameworks.

## Known Limitations

- No WebSocket reconnection — process exits on disconnect
- No multi-symbol support (single `btcusdt@depth` stream)
- No latency instrumentation on the live path
- Pipeline throughput matches sequential — multithreading does not improve this workload
- `std::thread::yield()` reduces spin iterations but CPU utilization was not profiled
- Benchmark results are environment-specific and use synthetic data only
- Recorded data without snapshots does not produce a verified synchronized book
- Mock server tests skip when local port binding is unavailable
