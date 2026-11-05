# Market Data Engine

A low-latency C++20 market-data engine that ingests live exchange data, reconstructs a local order book, and measures processing performance.

Built incrementally as a learning project for C++ systems engineering: networking, memory management, concurrency, and profiling.

## Current status

| Milestone | Description | Status |
|---|---|---|
| **M1.1** | Live WebSocket receiver | Complete |
| **M1.2** | Typed depth message parsing | Complete |
| **M2** | Order book reconstruction | Complete |
| **M2.1** | Order book data structure | Complete |
| **M2.2** | Asynchronous HTTPS client | Complete |
| **M2.3** | Synchronization engine | Complete |
| **M2.4** | Session integration | Complete |

## Architecture

M1.2 adds a typed parsing layer between the WebSocket receiver and output:

```
Binance WebSocket API                     market-data-engine
(stream.binance.com:9443)                 (single process, single thread)

  wss://...@depth ──────────────────────▸ Session
                    TLS + WebSocket         │
                    ~1 msg/sec              ├─▸ parse_depth_update()
                                            │     JSON → DepthUpdate struct
                                            │     decimal strings → scaled int64
                                            │     validates all fields
                                            │
                                            ├─▸ decoded summary → stderr
                                            │     symbol, U, u, bid/ask counts, E
                                            │
                                            ├─▸ [raw JSON → stdout if MARKET_DATA_RAW=1]
                                            │
                                            └─▸ stats tracking
                                          
                                          main()
                                            ├─▸ io_context::run()  (event loop)
                                            ├─▸ signal_set          (Ctrl-C)
                                            └─▸ error callback      (fatal → exit)
```

**Data flow:** Binance sends differential depth updates over a TLS WebSocket connection. `Session` receives each message into a bounded 1 MB buffer. `parse_depth_update()` parses the JSON into a typed `DepthUpdate` struct, converting decimal price/quantity strings to scaled `int64_t` values (scale 10^8) without floating-point. On success, a one-line decoded summary is printed to stderr. On failure, the parse error is logged and the invalid-message counter incremented. On Ctrl-C, `Session::stop()` sends a WebSocket Close frame and prints session statistics.

**Raw JSON debugging:** Set `MARKET_DATA_RAW=1` to print timestamped raw JSON to stdout alongside the decoded summary.

**Threading model:** Single-threaded. One `io_context::run()` call processes all async completions sequentially.

## Building

Requires macOS with Homebrew, CMake 3.20+, and a C++20 compiler.

```bash
# install dependencies
brew install boost ninja

# build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build

# run tests
./build/tests

# run the live receiver (Ctrl-C to stop)
./build/market-data-engine

# run with raw JSON output
MARKET_DATA_RAW=1 ./build/market-data-engine
```

## Project structure

```
src/
├── main.cpp              Entry point, io_context, signal handling, error callback
├── session.hpp/cpp       Async WebSocket session (connect, TLS, read loop)
├── depth_update.hpp/cpp  DepthUpdate types, strict JSON parser, shared parse helpers
├── order_book.hpp/cpp    OrderBook class (bid/ask maps, update procedure, snapshot application)
├── snapshot.hpp/cpp      Snapshot type and REST depth response parser
├── http_client.hpp/cpp   Async HTTPS client (persistent connection, lazy reconnection, TLS verification)
├── sync_engine.hpp/cpp   Snapshot sync state machine (buffering, retry, gap detection)
└── util.hpp/cpp          Timestamp formatting, scaled decimal parser and formatter
tests/
└── test_main.cpp         114 unit tests (Boost.Test)
docs/
├── DECISIONS.md          Engineering decisions log
└── LEARNING.md           C++ and systems concepts journal
GLOSSARY.md               Domain terminology
```

## Features

### M1.1 — Live WebSocket Receiver

- Async WebSocket connection to `wss://stream.binance.com:9443/ws/btcusdt@depth`
- TLS certificate validation (`verify_peer`) and hostname verification (`SSL_set1_host`)
- Automatic WebSocket ping/pong via Beast `timeout::suggested`
- Bounded receive buffer (1 MB) to prevent memory exhaustion
- Session diagnostics printed on shutdown (uptime, message count, bytes, invalid messages)
- Clean shutdown on SIGINT/SIGTERM with WebSocket Close frame
- Shutdown guards in every async callback to prevent operations after stop
- Fatal errors exit with nonzero status via error callback and signal cancellation

### M1.2 — Typed Depth Message Parsing

- Strongly typed `DepthUpdate` and `PriceLevel` structs with clear ownership
- Prices and quantities as signed 64-bit scaled integers (scale 10^8, no floating-point)
- Strict parsing: validates event type, symbol, timestamps, sequence IDs, and decimal format
- Rejects invalid sequence ranges (`U > u`), zero prices, negative quantities, overflow, and excess precision
- Atomic parse failure — returns `ParseError` variant, never a partial `DepthUpdate`
- Zero-quantity deletion updates accepted (Binance protocol for removing price levels)
- Raw JSON debugging via `MARKET_DATA_RAW=1` environment variable
- 90 unit tests covering scaled decimal parsing (including INT64_MAX boundary, negative zero), depth update parsing (including empty symbol, fractional overflow), snapshot parsing, order book operations, format_scaled, and M1.1 networking/lifecycle

### M2.1 — Order Book Data Structure

- `OrderBook` class with `std::map<int64_t, int64_t>` for bids (descending) and asks (ascending)
- Snapshot application: full book replacement with update ID tracking
- Differential update application per Binance protocol (Applied, Stale, Gap detection)
- Best bid/ask retrieval as `std::optional<std::pair<int64_t, int64_t>>`
- Crossed book detection (best bid >= best ask) with stderr warning
- `Snapshot` struct and `parse_snapshot()` parser for REST `/api/v3/depth` responses
- Zero-quantity snapshot entries silently skipped
- `format_scaled()` utility for converting scaled integers to human-readable decimals
- Shared `parse_levels()` and `get_uint64()` helpers extracted from M1.2 for reuse
- 35 new unit tests (90 total)

### M2.2 — Asynchronous HTTPS Client

- `HttpClient` class with async GET via Boost.Beast on the existing `io_context`
- Persistent HTTP/1.1 connection with lazy reconnection on failure
- TLS certificate-chain verification, hostname verification (`SSL_set1_host`), and SNI
- 10-second end-to-end deadline via `net::steady_timer` (configurable for tests)
- Centralized completion path — exactly one callback per request
- Overlapping `async_get()` calls rejected with busy error
- HTTP response body limit (2 MB) via `http::response_parser::body_limit`
- Connection-close semantics respected for connection reuse decisions
- Fresh TLS session state on reconnection via stream reconstruction
- `cancel()` for clean shutdown with explicit cancelled result
- 9 new deterministic tests using local mock HTTPS server (103 total)

### M2.3/M2.4 — Synchronization Engine and Session Integration

- `SyncEngine` class implementing the official Binance 7-step local order book sync protocol
- State machine: Buffering → SnapshotPending → Synchronized with resync on failure
- Bounded update buffer (`std::deque<DepthUpdate>`, max 1000 events, resync on overflow)
- Exponential backoff on stale snapshots (5 retries, 1s base, 2× multiplier)
- Sequence gap detection triggers full resynchronization
- Snapshot validation: stale check, bridging event detection, buffered event replay
- HTTP error, parse error, and transport failure handling with automatic resync
- Session integration: parsed depth updates routed through SyncEngine
- Best bid/ask with spread printed on every applied update when synchronized
- Graceful shutdown via `SyncEngine::stop()` cancelling HTTP requests and timers
- 11 new deterministic tests with mock HTTPS server (114 total)

## Known limitations

- No reconnection on WebSocket disconnect — process exits on connection error
- No multi-symbol support
- No latency measurement instrumentation
- Scale factor hardcoded to 10^8 — sufficient for all current Binance Spot pairs
- `timestamp_now()` uses `gmtime_r` (POSIX) — not portable to Windows
- Namespace aliases in `session.hpp` leak into includers
- Hostname mismatch integration test requires local port binding (skipped in sandboxed environments)

## Dependencies

| Library | Purpose |
|---|---|
| [Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/) | WebSocket protocol and HTTP upgrade |
| [Boost.Asio](https://www.boost.org/doc/libs/release/libs/asio/) | Async I/O, TCP, DNS, signal handling |
| [Boost.JSON](https://www.boost.org/doc/libs/release/libs/json/) | JSON parsing for depth update messages |
| [OpenSSL](https://www.openssl.org/) | TLS encryption, certificate verification |
| [Boost.Test](https://www.boost.org/doc/libs/release/libs/test/) | Unit testing framework |
