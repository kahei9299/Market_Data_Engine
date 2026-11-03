# Decisions

Architectural and implementation decisions for the market-data-engine project. Each entry records what was decided and why.

## M1.1 — Live WebSocket Receiver

### Networking library: Boost.Beast

Chose Beast (built on Asio) over lighter alternatives (libwebsockets, ixwebsocket, raw sockets). Beast teaches the Asio `io_context` and async patterns needed for later milestones (concurrency, multiple streams). The cost is a heavy dependency (~200MB installed via Homebrew).

### I/O model: asynchronous with `io_context::run()`

Chose async over synchronous blocking reads. Synchronous I/O is simpler to read but cannot be cleanly interrupted by signal handlers — a blocking `read()` won't wake until the next message arrives. Async I/O with `signal_set` handles Ctrl-C naturally and teaches the event-loop pattern early.

### Dependencies via Homebrew

Boost and OpenSSL installed via `brew install boost`. No vcpkg or Conan. Trades reproducibility and portability for simplicity on a single macOS development machine. Migrate to vcpkg when CI or cross-platform builds become necessary.

### Error handling: exit on error (no reconnection)

Connection errors print a clear message and exit. Reconnection with exponential backoff is deferred to a later milestone. This keeps M1.1 focused on the connection lifecycle.

### Stream: `btcusdt@depth` (differential depth)

Chose the 1-second differential depth stream over trade streams or partial-book snapshots. Diff updates carry sequence IDs (`U`, `u`) needed for order-book reconstruction in M2. Messages are ~1/second with variable-length bid/ask arrays.

### Endpoint: production (`stream.binance.com:9443`)

Using the production endpoint, not testnet. Read-only with a single connection — no risk of rate-limit issues. The host is a named constant for easy switching.

### Output: timestamp + raw JSON

Each message prints as `[ISO-8601-UTC] <raw JSON>` on one line. The timestamp is local receive time. Comparing it with Binance's `E` field (exchange event time) gives an approximate measure of message freshness — not isolated network latency, since it includes exchange-side queuing, serialization, and clock skew between the exchange and the local machine.

### Session class for async lifetime safety

A `Session` class using `std::enable_shared_from_this` owns the connection objects and async callbacks. Each callback captures a `shared_ptr<Session>`, preventing use-after-free when operations complete after the initiating scope exits. This is the standard Beast/Asio pattern, not an unnecessary abstraction.

### TLS: certificate validation and hostname verification

SSL context uses `set_default_verify_paths()` (system CA roots) and `verify_peer` (reject untrusted certificates). Two separate OpenSSL calls handle hostname security: `SSL_set_tlsext_host_name` sets SNI (tells the server which certificate to present), and `SSL_set1_host` configures certificate hostname verification (OpenSSL checks the certificate's SAN/CN matches the expected hostname during handshake). Beast's WebSocket timeout handles ping/pong automatically via `timeout::suggested(role_type::client)`.

### Build: CMake + Ninja, C++20

Minimal CMakeLists.txt. Ninja for fast incremental builds. C++20 for `std::chrono` improvements and modern language features.
