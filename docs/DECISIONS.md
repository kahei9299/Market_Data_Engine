# Decisions

Architectural and implementation decisions for the market-data-engine project. Each entry records what was decided and why.

## M1.1 — Live WebSocket Receiver

### Networking library: Boost.Beast

**Problem:** Need a C++ library for WebSocket over TLS.

**Options considered:**
- Boost.Beast (built on Asio) — full-featured, teaches patterns reusable across milestones
- libwebsockets — C library, lighter but no Asio integration
- ixwebsocket — simpler API but limited async control
- Raw sockets — maximum learning but impractical for production use

**Selected:** Boost.Beast. It teaches the Asio `io_context` and async callback patterns needed for later milestones (concurrency, multiple streams, custom allocators). The tradeoff is a heavy dependency (~200 MB installed via Homebrew).

**Revisit if:** dependency size becomes a deployment concern, or if Asio patterns prove unnecessary for the project's architecture.

### I/O model: asynchronous with `io_context::run()`

**Problem:** Synchronous blocking reads cannot be cleanly interrupted by signal handlers — a blocking `read()` won't wake until the next message arrives.

**Selected:** Async I/O with a single-threaded `io_context::run()` event loop. All operations (DNS, TCP, TLS, WebSocket, signals) register with the same `io_context` and complete via callbacks. This enables clean Ctrl-C handling via `signal_set` and teaches the event-loop pattern early.

**Rejected:** Synchronous I/O — simpler to read but requires workarounds for shutdown (socket timeouts or closing from a signal handler, both error-prone).

**Revisit if:** multi-threaded I/O is needed. The current strands are configured but single-threaded `run()` is sufficient for one stream.

### Dependencies via Homebrew

**Problem:** Boost and OpenSSL must be installed on the build machine.

**Selected:** Homebrew (`brew install boost ninja`). Simple, macOS-native, zero configuration.

**Rejected:** vcpkg (version-locked, portable, but slow bootstrap and more setup), Conan (similar to vcpkg, less common for C++ beginners).

**Tradeoff:** Not reproducible across machines. Dependency versions drift per Homebrew update.

**Revisit if:** CI is added, or the project needs to build on Linux or another developer's machine.

### Error handling: exit on error with nonzero status

**Problem:** When the WebSocket connection fails (DNS, TLS, disconnect), the application must terminate cleanly rather than hanging.

**Selected:** `Session::fail()` calls an error callback provided by `main()`. The callback sets `exit_code = EXIT_FAILURE` and cancels the `signal_set`, which allows `io_context::run()` to drain and return. No reconnection.

**Why the signal_set matters:** Without cancelling it, its pending `async_wait` keeps `io_context::run()` alive indefinitely after a fatal error.

**Rejected:** Reconnection with exponential backoff — adds state management complexity. Deferred to a later milestone.

**Revisit if:** the application needs to run unattended, or Binance's 24-hour connection limit is reached in practice.

### Bounded receive buffer (1 MB)

**Problem:** `beast::flat_buffer` grows without limit by default. A malformed or malicious message could exhaust memory.

**Selected:** Pass `kMaxBufferSize` (1 MB) to the `flat_buffer` constructor. If a message exceeds this, `async_read` fails with `buffer_overflow`.

**Why 1 MB:** Binance depth updates are typically 1–10 KB. 1 MB provides >100x headroom while capping worst-case memory use.

**Revisit if:** message sizes grow significantly (unlikely for depth updates), or if memory profiling shows the buffer allocation is wasteful.

### Shutdown guards in async callbacks

**Problem:** `stop()` cancels the resolver and closes the socket, but a successfully completed async operation can race with the cancellation. If `on_resolve` completes successfully just as `stop()` fires, the callback starts TCP connect on a closed socket.

**Selected:** Every callback checks `if (stopping_) return;` after the error check. This prevents advancing to the next connection stage after shutdown begins.

**Why cancellation alone isn't enough:** An operation that already completed successfully (e.g., DNS resolved) isn't affected by `resolver_.cancel()`. Its callback fires with no error and must check `stopping_` explicitly.

### Session class with `enable_shared_from_this`

**Problem:** Async callbacks reference `Session` members (socket, buffer, resolver). If `Session` is destroyed before callbacks fire, the result is use-after-free.

**Selected:** `Session` inherits from `std::enable_shared_from_this`. Each callback captures `shared_from_this()`, keeping the object alive until all operations complete. This is the standard Beast/Asio pattern.

**Rejected:** Stack-allocated session with manual lifetime management — fragile and error-prone with async operations.

### TLS: certificate validation and hostname verification

**Problem:** A TLS connection must verify the server's identity, not just encrypt the data.

**Selected:** Two-layer verification:
1. `ssl::context::set_verify_mode(verify_peer)` — validates the certificate chain against system CA roots
2. `SSL_set1_host()` — verifies the certificate's SAN/CN matches the expected hostname

**Why both are needed:** `verify_peer` alone checks that the certificate is signed by a trusted CA, but accepts any valid certificate (e.g., one for `evil.com`). `SSL_set1_host` adds hostname checking so only a certificate for `stream.binance.com` is accepted.

`SSL_set_tlsext_host_name()` is a separate concern: it configures SNI (Server Name Indication), telling the server which hostname the client wants so multi-domain servers present the correct certificate.

**Discovered during code review:** The initial implementation only called `SSL_set_tlsext_host_name`, which configures SNI but does not verify the hostname. This was a real security gap — a valid certificate for any domain would have been accepted.

### Structural validation only

**Problem:** Incoming messages are untrusted exchange data that must be validated.

**Selected:** `validate_depth_message()` checks top-level JSON structure: valid JSON, `"e": "depthUpdate"`, numeric `E`/`U`/`u`, array `b`/`a`. It does not validate bid/ask array element format, price/quantity string contents, or sequence ID ranges.

**Why structural only:** M1.1 prints raw JSON — it doesn't parse the data. Full content validation belongs in M1.2 when we extract price/quantity values and track sequencing.

### Stream: `btcusdt@depth` (differential depth)

**Problem:** Binance offers multiple stream types with different data shapes.

**Options considered:**
- `btcusdt@trade` — one message per trade, low volume, simple
- `btcusdt@depth` — incremental order book diffs at 1-second intervals with sequence IDs
- `btcusdt@depth20` — top-20 level snapshots, no sequencing

**Selected:** `btcusdt@depth`. The differential updates carry sequence IDs (`U`, `u`) needed for order-book reconstruction in M2. Messages arrive ~1/second with variable-length bid/ask arrays.

### Output: timestamp + raw JSON

Each message prints as `[ISO-8601-UTC] <raw JSON>` on one line. The timestamp is local receive time. Comparing it with Binance's `E` field (exchange event time) gives an approximate measure of message freshness — not isolated network latency, since it includes exchange-side queuing, serialization, and clock skew between the exchange and the local machine.

### Endpoint: production (`stream.binance.com:9443`)

Using the production endpoint, not testnet. Read-only with a single connection — well within Binance's 300-connections-per-5-minutes limit. The host is a named constant for easy switching.

### Build: CMake + Ninja with static library

`CMakeLists.txt` compiles non-main sources into a static library (`engine_core`). Both the main executable and the test executable link against it, avoiding compiling shared sources twice. Ninja is used for fast incremental builds. C++20 is required for `std::chrono` improvements and modern language features.
