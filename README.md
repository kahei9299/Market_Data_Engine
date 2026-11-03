# Market Data Engine

A low-latency C++20 market-data engine that ingests live exchange data, reconstructs a local order book, and measures processing performance.

Built incrementally as a learning project for C++ systems engineering: networking, memory management, concurrency, and profiling.

## Current status

### M1.1 — Live WebSocket Receiver ✅

Connects to Binance's public BTCUSDT differential depth stream over TLS WebSocket, receives and validates real-time order book update messages, and prints them with timestamps.

**What works:**
- Async WebSocket connection to `wss://stream.binance.com:9443/ws/btcusdt@depth`
- TLS certificate validation and hostname verification
- Automatic WebSocket ping/pong handling
- JSON structure validation of incoming depth updates
- Bounded receive buffer (1 MB max)
- Session diagnostics (message count, bytes, invalid messages, uptime)
- Clean shutdown on Ctrl-C with stats summary
- Error callback to notify the caller on connection failure
- 14 unit tests (timestamp formatting, message validation, buffer bounds)

**Not yet implemented:**
- Depth message parsing (bid/ask price/quantity extraction)
- Sequence gap detection (`U`/`u` tracking)
- Order book reconstruction
- Reconnection with backoff
- Latency measurement

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
├── main.cpp            Entry point, event loop, signal handling
├── session.hpp/cpp     Async WebSocket session (connect, TLS, read loop)
├── util.hpp/cpp        Timestamp formatting
└── validation.hpp/cpp  Depth message JSON validation
tests/
└── test_main.cpp       Unit tests (Boost.Test)
docs/
└── DECISIONS.md        Engineering decisions log
```

## Dependencies

- [Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/) — WebSocket and HTTP
- [Boost.Asio](https://www.boost.org/doc/libs/release/libs/asio/) — Async I/O and networking
- [Boost.JSON](https://www.boost.org/doc/libs/release/libs/json/) — JSON parsing
- [OpenSSL](https://www.openssl.org/) — TLS encryption
- [Boost.Test](https://www.boost.org/doc/libs/release/libs/test/) — Unit testing
