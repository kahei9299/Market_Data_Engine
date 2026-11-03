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

## M1.2 — Typed Depth Message Parsing

### Numeric representation: signed 64-bit scaled integers

**Problem:** Binance sends prices and quantities as decimal strings (e.g. `"78450.06000000"`). The engine needs a numeric type for arithmetic that is exact — no rounding.

**Options considered:**
- `double` — simple, native arithmetic, but IEEE 754 cannot represent all 8-decimal-place values exactly. `78450.06` is not representable; the nearest double is `78450.05999999999...`. Rounding errors accumulate in order-book reconstruction.
- `std::string` — no precision loss, but arithmetic requires re-parsing every operation. Impractical for an order book.
- `int64_t` with fixed scale 10^8 — exact representation of all Binance values. Arithmetic is integer arithmetic (fast, deterministic). Requires explicit scale tracking.
- Decimal library (e.g. `boost::multiprecision`) — exact, but heavy dependency for what 64-bit integers already solve.

**Selected:** `int64_t` scaled by `kScaleFactor = 100'000'000` (10^8), defined in `src/util.hpp`. `"78450.06000000"` becomes `7845006000000`. The scale matches Binance's `baseAssetPrecision` and `quotePrecision` of 8 for all current Spot pairs.

Prices are stored as signed `int64_t` (not unsigned) because signed arithmetic avoids unsigned underflow surprises in future subtraction operations (e.g. spread calculation). Parse-time validation rejects negative values.

**Tradeoff:** Every consumer must know the scale factor. Overflow is possible for values above ~92 billion at scale 10^8 — rejected at parse time.

**Revisit if:** A Binance Spot pair exceeds 8 decimal places (currently none do), or if 64-bit range proves insufficient for aggregated quantities.

### Parse result: `std::variant<DepthUpdate, ParseError>`

**Problem:** A failed parse must never expose a partially constructed `DepthUpdate`. The return type must make partial state impossible.

**Options considered:**
- `std::variant<DepthUpdate, ParseError>` — type-safe union. You have one or the other, never both. Access via `std::get_if` or `std::visit`.
- `struct { bool ok; DepthUpdate event; ParseError error; }` — allows reading `event` when `ok` is false.
- `std::expected<DepthUpdate, ParseError>` — cleaner monadic API, but C++23. Not available in C++20.
- Throw on parse failure — expensive for expected errors from untrusted network input.

**Selected:** `std::variant`. Defined in `src/depth_update.hpp`. The variant makes partial-state impossible at the type level. C++20 compatible.

**Revisit if:** C++23 is adopted — `std::expected` has a cleaner API for value-or-error.

### Decimal string parsing without floating-point

**Problem:** Converting `"78450.06000000"` to a scaled integer without going through `double`.

**Selected:** `parse_scaled_decimal()` in `src/util.cpp`. Algorithm:
1. Split at the decimal point.
2. Parse integer and fractional parts separately using `std::from_chars` (no locale, no allocation, no floating-point).
3. Check overflow before scaling: `integer > INT64_MAX / kScaleFactor` rejects.
4. Scale fractional part by padding with trailing zeros to 8 digits.
5. Combine: `integer * kScaleFactor + fractional`.

Rejects: >8 decimal places, negative values, empty input, non-numeric characters, trailing dot with no digits, integer overflow.

**Why `std::from_chars`:** It parses directly from a `string_view` without allocating a `std::string`, does not depend on locale settings, and reports exact parse boundaries (detects trailing garbage).

**Revisit if:** Performance profiling shows `from_chars` is a bottleneck (unlikely — it's highly optimized in libc++).

### Removed `validation.hpp/cpp` — parsing subsumes validation

**Problem:** M1.1's `validate_depth_message()` checked top-level JSON structure. M1.2's `parse_depth_update()` does the same checks and also extracts values.

**Selected:** Removed `validation.hpp` and `validation.cpp`. The parser validates structure as it parses — running a separate validation pass first would parse the JSON twice for every message. The M1.1 validation tests were migrated to the parser test suite.

### Debug output via `MARKET_DATA_RAW` environment variable

**Problem:** M1.1 printed raw JSON to stdout. M1.2 replaces this with a decoded summary on stderr. Raw output is still useful for debugging.

**Options considered:**
- Remove raw output entirely — use external tools if needed.
- Environment variable `MARKET_DATA_RAW=1` — runtime toggle, zero code complexity.
- Command-line flag `--raw` — requires argument parsing.
- Log-level system — more infrastructure than needed.

**Selected:** Environment variable, checked once at startup via `std::getenv`. When set, raw JSON is printed to stdout alongside the decoded summary on stderr.

**Revisit if:** A proper CLI argument parser is added for other reasons.

### Validation strictness

**Problem:** What to accept and reject when parsing untrusted exchange data.

**Selected policy:**
- Prices must be strictly positive (`> 0`). A zero-price order is meaningless.
- Quantities must be nonnegative (`>= 0`). Zero quantity is a valid deletion signal (see GLOSSARY.md).
- Negative values rejected including negative zero (`"-0"`, `"-0.00000000"`) — checked by inspecting the leading character before parsing.
- Empty symbols rejected.
- Sequence range: `U <= u`. If `U > u`, the update is malformed.
- Each price-level entry must be a 2-element array of strings. Numeric JSON values are rejected — Binance always sends strings.
- Decimal precision capped at 8 digits. More digits are rejected rather than silently truncated.
- Integer overflow is checked before scaling and before fractional addition. The maximum representable value is `92233720368.54775807` (INT64_MAX at scale 10^8).

Invalid messages increment `stats_.invalid_messages` and log the error with a truncated preview of the raw data. The session continues processing subsequent messages.
