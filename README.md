# Market Data Engine

A low-latency C++20 market-data engine that ingests live exchange data, reconstructs a local order book, and measures processing performance.

Built incrementally as a learning project for C++ systems engineering: networking, memory management, concurrency, and profiling.

## Current status

| Milestone | Description | Status |
|---|---|---|
| **M1.1** | Live WebSocket receiver | Complete |
| **M1.2** | Typed depth message parsing | Complete |
| **M2** | Order book reconstruction | Planned |

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
├── depth_update.hpp/cpp  DepthUpdate types and strict JSON parser
└── util.hpp/cpp          Timestamp formatting, scaled decimal parser
tests/
└── test_main.cpp         53 unit tests (Boost.Test)
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
- 53 unit tests covering scaled decimal parsing (including INT64_MAX boundary, negative zero), depth update parsing (including empty symbol, fractional overflow), and M1.1 networking/lifecycle

## Known limitations

- No reconnection on disconnect — the process exits on any connection error
- No sequence gap detection (`U`/`u` continuity tracking across messages)
- No order book reconstruction
- No latency measurement
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
