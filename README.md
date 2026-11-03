# Market Data Engine

A low-latency C++20 market-data engine that ingests live exchange data, reconstructs a local order book, and measures processing performance.

Built incrementally as a learning project for C++ systems engineering: networking, memory management, concurrency, and profiling.

## Current status

| Milestone | Description | Status |
|---|---|---|
| **M1.1** | Live WebSocket receiver | Complete |
| **M1.2** | Depth message parsing and sequence tracking | Planned |
| **M2** | Order book reconstruction | Planned |

## Architecture

M1.1 implements a single-stream WebSocket receiver:

```
Binance WebSocket API                     market-data-engine
(stream.binance.com:9443)                 (single process, single thread)

  wss://...@depth ──────────────────────▸ Session
                    TLS + WebSocket         │
                    ~1 msg/sec              ├─▸ validate_depth_message()
                                            ├─▸ [timestamp] raw JSON → stdout
                                            └─▸ stats tracking
                                          
                                          main()
                                            ├─▸ io_context::run()  (event loop)
                                            ├─▸ signal_set          (Ctrl-C)
                                            └─▸ error callback      (fatal → exit)
```

**Data flow:** Binance sends differential depth updates over a TLS WebSocket connection. `Session` receives each message into a bounded 1 MB buffer, validates the JSON structure (event type, required fields), prints it with a UTC timestamp, and loops. On Ctrl-C, `Session::stop()` sends a WebSocket Close frame and prints session statistics. On fatal error, the error callback cancels the signal handler and the process exits with a nonzero status.

**Threading model:** Single-threaded. One `io_context::run()` call processes all async completions sequentially. Strands are configured for future multi-thread safety but currently unused.

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
```

## Project structure

```
src/
├── main.cpp            Entry point, io_context, signal handling, error callback
├── session.hpp/cpp     Async WebSocket session (DNS, TCP, TLS, WS, read loop)
├── util.hpp/cpp        ISO-8601 UTC timestamp formatting
└── validation.hpp/cpp  Structural JSON validation for depth updates
tests/
└── test_main.cpp       20 unit tests (Boost.Test)
docs/
├── DECISIONS.md        Engineering decisions log
└── LEARNING.md         C++ and systems concepts journal
```

## Features (M1.1)

- Async WebSocket connection to `wss://stream.binance.com:9443/ws/btcusdt@depth`
- TLS certificate validation (`verify_peer`) and hostname verification (`SSL_set1_host`)
- Automatic WebSocket ping/pong via Beast `timeout::suggested`
- Structural JSON validation of incoming depth updates (top-level fields only — does not validate bid/ask array contents or sequence ranges)
- Bounded receive buffer (1 MB) to prevent memory exhaustion
- Session diagnostics printed on shutdown (uptime, message count, bytes, invalid messages)
- Clean shutdown on SIGINT/SIGTERM with WebSocket Close frame
- Shutdown guards in every async callback to prevent operations after stop
- Fatal errors exit with nonzero status via error callback and signal cancellation
- 20 unit tests covering validation, timestamps, TLS configuration, lifecycle cleanup, and hostname mismatch rejection

## Known limitations

- No reconnection on disconnect — the process exits on any connection error
- No parsing of bid/ask data (price/quantity extraction deferred to M1.2)
- No sequence gap detection (`U`/`u` continuity tracking deferred to M1.2)
- No latency measurement
- `timestamp_now()` uses `gmtime_r` (POSIX) — not portable to Windows
- Namespace aliases in `session.hpp` leak into includers
- Hostname mismatch integration test requires local port binding (skipped in sandboxed environments)

## Dependencies

| Library | Purpose |
|---|---|
| [Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/) | WebSocket protocol and HTTP upgrade |
| [Boost.Asio](https://www.boost.org/doc/libs/release/libs/asio/) | Async I/O, TCP, DNS, signal handling |
| [Boost.JSON](https://www.boost.org/doc/libs/release/libs/json/) | JSON parsing for message validation |
| [OpenSSL](https://www.openssl.org/) | TLS encryption, certificate verification |
| [Boost.Test](https://www.boost.org/doc/libs/release/libs/test/) | Unit testing framework |
