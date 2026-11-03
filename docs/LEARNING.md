# Learning Journal

C++ and systems engineering concepts encountered in the project, organized by milestone. Each entry references the actual implementation and follows the structure: What, Where, Why, How, and Pitfalls.

## M1.1 — Live WebSocket Receiver

### Asio's async event loop (`io_context`)

**What:** `boost::asio::io_context` is an event loop that drives asynchronous I/O operations. You register work (async operations), then call `run()` to process completions.

**Where:** Created in [`src/main.cpp`](../src/main.cpp) (`main()` function):

```cpp
net::io_context ioc;
// ... register async work ...
ioc.run();
```

**Why:** A single-threaded event loop lets us handle networking, signal handling, and timeouts without threads or blocking calls. All operations share one `io_context` so they can be coordinated.

**How:** Async operations like `async_resolve` and `async_read` *initiate* I/O work and return immediately — the OS begins processing (e.g., sending a DNS query), but the completion handler does not fire yet. `ioc.run()` blocks and dispatches completion handlers one at a time as operations finish. It returns when no pending operations remain.

**Pitfalls:**
- `ioc.run()` returns when there's no work left. A pending `signal_set::async_wait` keeps it alive (see [`src/main.cpp`](../src/main.cpp)). If a fatal error fires but the signal set isn't cancelled, `run()` never returns. Our error callback cancels the signal set (`signals.cancel()`) to allow draining.
- Calling `run()` from multiple threads would dispatch handlers concurrently — our single-threaded usage avoids this. The strands configured in `Session` (see below) are preparation for future multi-threading.

### The async callback chain

**What:** The connection is established through a sequence of async operations, each starting the next via its completion handler.

**Where:** [`src/session.cpp`](../src/session.cpp), methods `run()` through `on_read()`:

```
run() → on_resolve → on_connect → on_ssl_handshake → on_handshake → do_read ↔ on_read
```

**Why:** Each network operation (DNS, TCP, TLS, WebSocket) must complete before the next can begin. Async chaining lets the event loop drive progress without blocking.

**How:** Each method calls an `async_*` function with `bind_front_handler(&Session::on_next, shared_from_this())`. The handler fires when the operation completes, checks for errors, then starts the next stage. For example, in [`src/session.cpp`](../src/session.cpp) `on_resolve`:

```cpp
void Session::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec)
        return fail(ec, "resolve");
    if (stopping_)
        return;

    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(10));
    beast::get_lowest_layer(ws_).async_connect(
        results,
        beast::bind_front_handler(&Session::on_connect, shared_from_this()));
}
```

**Pitfalls:**
- This is not recursion. `do_read()` registers an async read and returns immediately. `on_read` is invoked later by `ioc.run()`. The call stack never grows — verified by the fact that `do_read` contains only `async_read` (no direct call to `on_read`).

### Object lifetimes and `enable_shared_from_this`

**What:** `std::enable_shared_from_this<T>` allows a class to create `shared_ptr<T>` instances pointing to itself from within its own member functions. This keeps the object alive for the duration of async operations.

**Where:** [`src/session.hpp`](../src/session.hpp), class `Session`:

```cpp
class Session : public std::enable_shared_from_this<Session> {
```

Created in [`src/main.cpp`](../src/main.cpp):

```cpp
auto session = std::make_shared<Session>(ioc, ctx, kHost, kPort, kPath, ...);
```

**Why:** Async callbacks reference `Session` members (`ws_`, `buffer_`, `resolver_`). If `Session` were destroyed while callbacks are pending, the program would access freed memory. Each callback captures `shared_from_this()`, which returns a `shared_ptr<Session>` that prevents destruction until the callback completes.

**How:** `enable_shared_from_this` adds a hidden `weak_ptr` inside `Session`. When a `shared_ptr<Session>` is first created (via `make_shared`), the `weak_ptr` is initialized. Calling `shared_from_this()` promotes this `weak_ptr` to a `shared_ptr`, incrementing the reference count. Each pending callback holds one `shared_ptr`. When all callbacks finish, the count drops to zero and `Session` is destroyed.

**Pitfalls:**
- `shared_from_this()` requires that the object is already managed by a `shared_ptr`. If you create `Session` on the stack or with raw `new`, calling `shared_from_this()` is undefined behavior prior to C++17. Since C++17 (which we use — C++20), it throws `std::bad_weak_ptr`. Our code always uses `make_shared`, which initializes the internal `weak_ptr` correctly.
- There is no compile-time enforcement. A private constructor with a static `create()` factory would prevent misuse but adds complexity we don't need with a single call site.

### Layered stream architecture

**What:** Beast stacks protocol layers as nested template types, each wrapping the one below.

**Where:** [`src/session.hpp`](../src/session.hpp), member `ws_`:

```cpp
websocket::stream<ssl::stream<beast::tcp_stream>> ws_;
```

Reading inside-out: `beast::tcp_stream` (TCP socket with timeout support), wrapped by `ssl::stream` (TLS encryption), wrapped by `websocket::stream` (WebSocket framing).

**Why:** Each layer handles one protocol concern. Code operates at the appropriate level — `ws_.async_read()` for WebSocket messages, `ws_.next_layer()` for TLS configuration, `beast::get_lowest_layer(ws_)` for TCP timeouts and socket operations.

**How:** `ws_.async_read()` reads a WebSocket frame, which decrypts via TLS, which reads TCP bytes from the kernel. Each layer delegates downward transparently.

Actual usage in [`src/session.cpp`](../src/session.cpp):
- `ws_.next_layer()` — accesses the `ssl::stream` for TLS handshake and SNI/hostname configuration (in `on_connect`)
- `beast::get_lowest_layer(ws_)` — accesses the `tcp_stream` for connection timeouts (in `on_resolve`, `on_connect`) and socket close (in `stop()`)
- `ws_.async_read()` — reads complete WebSocket messages (in `do_read`)
- `ws_.async_handshake()` — sends the HTTP Upgrade request (in `on_ssl_handshake`)

**Pitfalls:**
- Timeouts must be managed at the right layer. TCP-level timeouts (`expires_after`) are set during connect, then disabled (`expires_never`) once the WebSocket layer takes over with its own idle/ping timeout via `timeout::suggested`. Setting both would cause conflicts.

### Buffer ownership and `consume()`

**What:** `beast::flat_buffer` is a contiguous, growable byte buffer that Beast reads into. It must be explicitly consumed after each read to free space.

**Where:** [`src/session.hpp`](../src/session.hpp) (declaration) and [`src/session.cpp`](../src/session.cpp) (`on_read` method):

```cpp
// session.hpp — member declaration:
beast::flat_buffer buffer_;

// session.cpp — constructor, bounded to 1 MB:
buffer_(kMaxBufferSize),

// session.cpp — in on_read:
auto data = beast::buffers_to_string(buffer_.data());
// ... use data ...
buffer_.consume(bytes_transferred);
```

**Why:** Reusing one buffer across reads avoids per-message heap allocation. Bounding it to `kMaxBufferSize` (1 MB, defined in [`src/session.hpp`](../src/session.hpp)) prevents a malformed message from exhausting memory.

**How:** `async_read` writes into the buffer's output sequence and commits the bytes. `buffer_.data()` returns a view of the committed bytes. `buffers_to_string` copies them into a `std::string` (the one allocation per message). `buffer_.consume(bytes_transferred)` advances the read pointer, marking those bytes as processed and freeing internal space for the next read.

**Pitfalls:**
- Forgetting `consume()` causes the committed region to grow by one message per read. `buffer_.data()` would return the concatenation of all prior messages. Eventually the buffer reaches `max_size()` and `async_read` fails with `beast::error::buffer_overflow`.
- `buffers_to_string` performs a copy. For M1.1 (printing JSON), this is acceptable. Future milestones that parse in-place may operate on the buffer view directly to avoid the copy.

### TLS: SNI vs. hostname verification

**What:** Two separate OpenSSL functions handle different aspects of TLS hostname security. Confusing them is a real security bug.

**Where:** [`src/session.cpp`](../src/session.cpp), `on_connect` method. TLS context configured in [`src/main.cpp`](../src/main.cpp), `main()`.

**Why:** A TLS connection must verify the server's identity, not just encrypt data. This requires both: telling the server which certificate to present (SNI), and verifying that the presented certificate actually belongs to the expected host.

**How:** Three separate mechanisms work together:

| Mechanism | Function | Location | Purpose |
|---|---|---|---|
| CA chain validation | `ctx.set_verify_mode(ssl::verify_peer)` | `main.cpp` | Reject certificates not signed by a trusted CA |
| SNI | `SSL_set_tlsext_host_name(handle, host)` | `session.cpp`, `on_connect` | Tell the server which hostname we want |
| Hostname verification | `SSL_set1_host(handle, host)` | `session.cpp`, `on_connect` | Reject certificates whose SAN/CN doesn't match |

**Pitfalls — learned during code review:**
- The initial M1.1 implementation only called `SSL_set_tlsext_host_name` (SNI). This tells the server which certificate to present but does **not** verify the hostname. With only `verify_peer`, any valid certificate from a trusted CA would be accepted — including one for `evil.com`. Adding `SSL_set1_host` was the fix.
- Both `SSL_set_tlsext_host_name` and `SSL_set1_host` operate on the OpenSSL `SSL*` native handle accessed via `ssl_stream.native_handle()`. These are C functions because Boost.Asio doesn't wrap them.
- This is verified by the `hostname_mismatch_rejected_by_local_server` test in [`tests/test_main.cpp`](../tests/test_main.cpp), which connects to a local TLS server whose certificate SAN doesn't match and confirms the handshake is rejected.

### Async cancellation and shutdown races

**What:** Cancelling async operations is a request, not a guarantee. A successfully completed operation can race with the cancellation and still fire its callback with no error.

**Where:** [`src/session.cpp`](../src/session.cpp), `stop()` method and every `on_*` callback.

**Why — learned during code review:** `stop()` cancels the resolver and closes the socket. But if an operation already completed successfully (e.g., DNS resolved) before `stop()` ran, its callback fires with no error and would start the next connection stage — potentially on a socket that `stop()` already closed.

**How:** `stop()` does three things:

```cpp
void Session::stop() {
    if (stopping_)
        return;
    stopping_ = true;
    print_stats();

    resolver_.cancel();

    if (ws_.is_open()) {
        ws_.async_close(websocket::close_code::normal, ...);
    } else {
        beast::error_code ec;
        beast::get_lowest_layer(ws_).socket().close(ec);
    }
}
```

1. Sets `stopping_ = true` — a flag every callback checks.
2. `resolver_.cancel()` — requests cancellation of pending DNS lookups.
3. Closes the socket or sends WebSocket Close depending on connection state.

Every callback checks `stopping_` after the error check:

```cpp
void Session::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec)
        return fail(ec, "resolve");
    if (stopping_)
        return;
    // ... proceed
}
```

Cancellation has three possible outcomes per operation:
1. **Not yet started** — completes with an error (often `operation_aborted`, but not guaranteed).
2. **In progress** — closing the socket interrupts it; the callback fires with an OS-dependent error.
3. **Already completed** — cancellation has no effect. The callback fires with no error. Only `stopping_` prevents it from advancing.

**Pitfalls:**
- Never assume cancellation produces `operation_aborted`. The specific error code depends on timing and the OS.
- `fail()` suppresses `operation_aborted` *and* any error when `stopping_` is true (see [`src/session.cpp`](../src/session.cpp), `fail` method). This prevents spurious error logging during shutdown.
- Verified by the `stop_during_resolve_does_not_advance` test in [`tests/test_main.cpp`](../tests/test_main.cpp), which calls `run()` then `stop()` immediately and verifies the connection never advanced (stats show never-connected).

### Error handling: `error_code` vs. exceptions

**What:** C++ offers two error-reporting styles. Our project uses both, for different situations.

**Where:** Exceptions in [`src/main.cpp`](../src/main.cpp) (`try/catch` around `main`). Error codes in [`src/session.cpp`](../src/session.cpp) (every async callback) and [`src/depth_update.cpp`](../src/depth_update.cpp) (`json::parse`).

**Why:** Exceptions suit fatal setup errors where recovery is impossible. Error codes suit async callbacks and input validation where errors are expected during normal operation.

**How:**

Exceptions — for construction and setup that should not fail in normal operation:

```cpp
// main.cpp — ssl::context constructor throws if SSL library is broken:
ssl::context ctx(ssl::context::tlsv12_client);
```

Error codes — for async callbacks where errors are a normal flow:

```cpp
// session.cpp — every on_* callback receives an error_code:
void Session::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec)
        return fail(ec, "resolve");
```

Error codes — for parsing untrusted input:

```cpp
// depth_update.cpp — non-throwing parse, because invalid JSON is expected:
boost::system::error_code ec;
auto jv = json::parse(data, ec);
if (ec)
    return {false, "invalid JSON: " + ec.message()};
```

**Pitfalls:**
- Asio async operations always report results via `error_code` in their completion handler. There is no throwing variant for async operations — you must check `ec`.
- The `try/catch` in `main()` is a safety net for constructors and `ioc.run()` internals, not for normal async errors.

### `std::move` and sink parameters

**What:** `std::move` casts an lvalue to an rvalue reference, enabling move semantics — transferring ownership of resources instead of copying them.

**Where:** [`src/session.cpp`](../src/session.cpp), `Session` constructor:

```cpp
Session::Session(net::io_context& ioc, ssl::context& ctx,
                 std::string host, std::string port, std::string path,
                 ErrorCallback on_error)
    : resolver_(net::make_strand(ioc)),
      ws_(net::make_strand(ioc), ctx),
      buffer_(kMaxBufferSize),
      host_(std::move(host)),
      port_(std::move(port)),
      path_(std::move(path)),
      on_error_(std::move(on_error)) {}
```

**Why:** The parameters `host`, `port`, `path` are taken by value (not `const&`). This is the "sink parameter" idiom: the caller can pass a temporary (zero copies — constructed directly) or an lvalue (one copy into the parameter). Either way, `std::move` transfers the parameter's resources into the member.

**How:** `std::move(host)` casts `host` to `std::string&&`. This selects `host_`'s move constructor, which steals the heap-allocated buffer pointer from `host` instead of allocating new memory and copying characters. For short strings (typically ≤ 22 bytes on libc++), the implementation uses Small String Optimization (SSO) — the characters are stored inline in the object, so a "move" copies the inline storage since there is no heap allocation to transfer.

**Pitfalls:**
- After a move, the source is in a **valid but unspecified** state (C++ standard [lib.types.movedfrom]). It can be destroyed or assigned to, but its value is not guaranteed to be empty. Never read a moved-from object.
- Moving `on_error_` (a `std::function`) follows the same rules — the callable is transferred, and the source may or may not be empty afterward.

### Anonymous namespaces

**What:** An unnamed namespace restricts symbols to the current translation unit (`.cpp` file), preventing name collisions and enforcing internal linkage.

**Where:** [`src/depth_update.cpp`](../src/depth_update.cpp) uses an anonymous namespace to contain the internal helpers `make_error`, `get_uint64`, and `parse_levels` — these are implementation details of the parser, not part of its public API. `timestamp_now()` was also originally in an anonymous namespace inside `session.cpp` before being moved to [`src/util.hpp`](../src/util.hpp) / [`src/util.cpp`](../src/util.cpp) so that tests could access it.

**Why:** When a helper function is only needed within one `.cpp` file, an anonymous namespace prevents other files from seeing or colliding with it.

**How (illustrative — not current project code):**

```cpp
// In example.cpp — not visible to other translation units:
namespace {
int internal_helper() { return 42; }
}
```

This is the C++ equivalent of the C `static` keyword for free functions. The technique applies whenever a function is an implementation detail of a single file.

**Pitfalls:**
- Symbols in anonymous namespaces have internal linkage. If the same function name appears in two different `.cpp` files inside anonymous namespaces, they are separate functions — no conflict, but also no sharing.

### `std::chrono` time handling

**What:** C++ provides type-safe clocks and durations for time measurement.

**Where:**
- `system_clock` in [`src/util.cpp`](../src/util.cpp), `timestamp_now()` function
- `steady_clock` in [`src/session.hpp`](../src/session.hpp), `SessionStats::connected_at` member

**Why:** Two clocks serve fundamentally different purposes:

| Clock | Use in project | Properties |
|---|---|---|
| `system_clock` | Wall-clock timestamps for output | Can jump (NTP adjustments, manual changes) |
| `steady_clock` | Uptime measurement | Monotonic — never jumps backward |

**How:** `timestamp_now()` in [`src/util.cpp`](../src/util.cpp) uses `system_clock::now()` for human-readable timestamps. The millisecond extraction uses `duration_cast<milliseconds>(...) % 1000` because `to_time_t` truncates to whole seconds. `gmtime_r` converts to UTC broken-down time — the `_r` suffix indicates the reentrant (thread-safe) variant that writes to a caller-provided struct.

`SessionStats::connected_at` uses `steady_clock` for uptime. In [`src/session.cpp`](../src/session.cpp), `print_stats()` computes elapsed time as `steady_clock::now() - stats_.connected_at` and casts to seconds. If `connected_at` was never set (default-constructed `time_point{}`), `print_stats` detects this and prints "never connected" instead.

**Pitfalls:**
- `gmtime_r` is POSIX, not ISO C++. It's available on macOS and Linux but not portable to Windows (use `_gmtime64_s` there).
- `steady_clock` is guaranteed monotonic by the C++ standard. `system_clock` is not — using it for elapsed time can produce negative durations if the system clock is adjusted backward.

### CMake: static libraries and link propagation

**What:** CMake organizes build targets (executables, libraries) and manages their dependencies.

**Where:** [`CMakeLists.txt`](../CMakeLists.txt):

```cmake
add_library(engine_core STATIC
    src/session.cpp
    src/util.cpp
    src/depth_update.cpp
)

target_include_directories(engine_core PUBLIC src)

target_link_libraries(engine_core
    PUBLIC
        Boost::system
        Boost::json
        OpenSSL::SSL
        OpenSSL::Crypto
)

add_executable(market-data-engine src/main.cpp)
target_link_libraries(market-data-engine PRIVATE engine_core)

add_executable(tests tests/test_main.cpp)
target_link_libraries(tests PRIVATE engine_core Boost::unit_test_framework)
```

**Why:** Compiling `session.cpp`, `util.cpp`, and `depth_update.cpp` into a static library (`engine_core`) avoids compiling them twice — once for the main executable, once for tests.

**How:** `add_library(... STATIC ...)` compiles the sources into `libengine_core.a`. Both `market-data-engine` and `tests` link against it. `PUBLIC` on `target_link_libraries` means dependents inherit those libraries — the test executable links `engine_core` and automatically gets Boost and OpenSSL. `PRIVATE` means the dependency stops at that target. `target_include_directories(... PUBLIC src)` lets includers write `#include "session.hpp"` instead of `#include "src/session.hpp"`.

**Pitfalls:**
- `PUBLIC` vs `PRIVATE` matters. If `engine_core` linked Boost as `PRIVATE`, the test executable would get linker errors because it wouldn't inherit the Boost libraries.
- `find_package(Boost REQUIRED COMPONENTS system json unit_test_framework)` finds all three components. If any is missing, CMake fails at configure time with a clear error.

## M1.2 — Typed Depth Message Parsing

### Fixed-point arithmetic with scaled integers

**What:** Representing decimal values as integers multiplied by a fixed power of 10, avoiding floating-point entirely. Our scale factor is 10^8 (`kScaleFactor` in [`src/util.hpp`](../src/util.hpp)), so `"78450.06000000"` becomes `7845006000000`.

**Where:** `parse_scaled_decimal()` in [`src/util.cpp`](../src/util.cpp). The constants `kDecimalScale` (8) and `kScaleFactor` (100,000,000) are defined in [`src/util.hpp`](../src/util.hpp). The parsed values are stored as `int64_t` fields in `PriceLevel` ([`src/depth_update.hpp`](../src/depth_update.hpp)).

**Why:** Floating-point (`double`) cannot represent many decimal fractions exactly. `0.1` in IEEE 754 is `0.1000000000000000055511151231257827021181583404541015625`. In a financial context, accumulated rounding errors would corrupt order-book state. Scaled integers are exact: `0.1 * 10^8 = 10000000` with no approximation.

**How:** `parse_scaled_decimal` in [`src/util.cpp`](../src/util.cpp) works in three stages:

```cpp
// 1. Split at the decimal point
auto dot = s.find('.');
std::string_view int_part = s.substr(0, dot);
std::string_view frac_part = s.substr(dot + 1);

// 2. Parse each part as an integer using std::from_chars
int64_t integer = 0;
auto [p1, ec1] = std::from_chars(int_part.data(), int_part.data() + int_part.size(), integer);

// 3. Combine: integer * scale + fractional (padded to kDecimalScale digits)
int64_t result = integer * kScaleFactor;
// frac_part "06" → 06 * 10^6 = 6000000
result += frac;
```

Overflow is checked before scaling:

```cpp
static constexpr int64_t kMaxInteger =
    std::numeric_limits<int64_t>::max() / kScaleFactor;
if (integer > kMaxInteger)
    return ScaledParseError{"integer part overflows scaled representation"};
```

This prevents `integer * kScaleFactor` from wrapping. The maximum representable integer part is `92233720368` (≈ 92 billion), well above any realistic price.

**Pitfalls:**
- The scale is hardcoded to 10^8. If Binance introduces a symbol with more than 8 decimal places, the parser rejects it. This is intentional — silent truncation would be worse. The constant can be promoted to a parameter when multi-symbol support is needed.
- `int64_t` is signed. Prices and quantities are validated as nonnegative at parse time (in `parse_levels`, [`src/depth_update.cpp`](../src/depth_update.cpp)). Signed arithmetic avoids unsigned underflow surprises in future subtraction operations (e.g., order-book deltas).

### `std::variant` for type-safe error handling

**What:** `std::variant<A, B>` holds exactly one of its alternative types at any time. We use it to represent parse results that are either a valid value or an error — with no possibility of accessing a partially constructed success.

**Where:** Two variant types are defined:
- `ScaledResult = std::variant<int64_t, ScaledParseError>` in [`src/util.hpp`](../src/util.hpp)
- `ParseResult = std::variant<DepthUpdate, ParseError>` in [`src/depth_update.hpp`](../src/depth_update.hpp)

Consumed in [`src/session.cpp`](../src/session.cpp) (`on_read`) and throughout [`src/depth_update.cpp`](../src/depth_update.cpp) (`parse_levels`, `parse_depth_update`).

**Why:** Unlike a struct with `bool ok` + both fields, a variant cannot hold both alternatives simultaneously. You cannot accidentally read the `DepthUpdate` from a variant that contains a `ParseError` — `std::get` would throw `std::bad_variant_access`, and `std::get_if` returns `nullptr`.

**How:** The pattern used throughout the parser:

```cpp
// depth_update.cpp — check for error, extract value:
auto price_result = parse_scaled_decimal(price_str);
if (auto* err = std::get_if<ScaledParseError>(&price_result))
    return ParseError{...};   // propagate error
auto price = std::get<int64_t>(price_result);  // safe — we know it's not an error

// session.cpp — branch on success vs failure:
auto result = parse_depth_update(data);
if (auto* err = std::get_if<ParseError>(&result)) {
    // handle error
} else {
    auto const& update = std::get<DepthUpdate>(result);
    // use update
}
```

`std::get_if<T>(&v)` returns a `T*` if the variant holds `T`, or `nullptr` otherwise. It never throws, making it suitable for expected-error paths. `std::get<T>(v)` throws `bad_variant_access` if the variant doesn't hold `T` — only use it after confirming the alternative with `get_if` or `holds_alternative`.

**Pitfalls:**
- `std::variant` has overhead: it stores a type index alongside the value, and `get_if`/`get` check it at runtime. For a parser called once per message (~1/second), this is negligible. For a hot inner loop, consider a different pattern.
- A `variant` with two types that share a common base or implicit conversion can be ambiguous at construction. Our types (`DepthUpdate` vs `ParseError`, `int64_t` vs `ScaledParseError`) are unrelated, so there's no ambiguity.

### `std::from_chars` for decimal parsing

**What:** A C++17 low-level number parsing function that converts a character range to a numeric type. It doesn't allocate, doesn't throw, and doesn't depend on locale.

**Where:** [`src/util.cpp`](../src/util.cpp), `parse_scaled_decimal` function — called twice per invocation (integer part and fractional part):

```cpp
int64_t integer = 0;
auto [p1, ec1] = std::from_chars(int_part.data(),
                                  int_part.data() + int_part.size(),
                                  integer);
if (ec1 != std::errc{} || p1 != int_part.data() + int_part.size())
    return ScaledParseError{"invalid integer part"};
```

**Why alternatives are worse:**
- `std::strtol` / `atoi`: require a null-terminated string (can't use `string_view` directly), set `errno` (global state, not thread-safe without care), and `atoi` has undefined behavior on overflow.
- `std::stod` / `atof`: go through floating-point, which we explicitly avoid for precision.
- `std::istringstream`: allocates, slow, locale-dependent (a German locale parses `"1,5"` as 1.5).

`from_chars` takes a `[begin, end)` pointer pair, writes the result into an output parameter, and returns a `from_chars_result` with a pointer to where parsing stopped (`ptr`) and an error code (`ec`). It's the fastest standard way to parse integers from text.

**How:** The return type uses C++17 structured bindings:

```cpp
auto [p1, ec1] = std::from_chars(...);
```

This destructures `from_chars_result{const char* ptr; std::errc ec;}` into two local variables. We check both `ec1 != std::errc{}` (parse error like overflow or invalid characters) and `p1 != end` (didn't consume all characters — means trailing garbage like `"123abc"`).

**Pitfalls:**
- `std::from_chars` for floating-point types (`float`, `double`) is not supported by all compilers/standard libraries as of C++20. We only use the integer overload, which is universally available.
- The function does not skip whitespace. Input like `" 123"` fails. This is correct for our use case — Binance values have no leading/trailing spaces.

### Atomic parse failure

**What:** A parse operation that either succeeds completely or fails completely — never produces a partially constructed result.

**Where:** `parse_depth_update()` in [`src/depth_update.cpp`](../src/depth_update.cpp).

**Why:** If the parser returned a `DepthUpdate` with valid bids but an error in asks, downstream code could process corrupted data. In a market-data system, applying half an update to the order book would leave it in an inconsistent state.

**How:** The function validates and extracts every field into local variables before constructing the `DepthUpdate`. If any field fails, it returns a `ParseError` immediately via early return. The `DepthUpdate` is only constructed in the final `return` statement, after all validation has passed:

```cpp
ParseResult parse_depth_update(std::string_view data) {
    // ... validate and extract each field into locals ...
    // Any failure returns ParseError before reaching here.

    return DepthUpdate{
        std::string(s_it->value().get_string()),
        std::get<uint64_t>(event_time),
        U, u,
        std::move(std::get<std::vector<PriceLevel>>(bids)),
        std::move(std::get<std::vector<PriceLevel>>(asks)),
    };
}
```

The `DepthUpdate` is constructed with aggregate initialization — all fields are set in one expression. There is no intermediate state where some fields are populated and others aren't.

**Pitfalls:**
- This pattern relies on returning `std::variant` — the function can only return one alternative. If it returned a struct with `ok` + `event` + `error`, a bug could accidentally set `ok = true` with partially filled fields.
- The `parse_levels` helper also uses the same pattern — it returns `std::variant<std::vector<PriceLevel>, ParseError>`, ensuring an entire side (bids or asks) is either fully parsed or not at all.

### `std::string_view` for zero-copy parameter passing

**What:** A non-owning read-only view of a contiguous character sequence — a pointer and a length. It avoids copying string data just to read it.

**Where:**
- `parse_scaled_decimal(std::string_view s)` in [`src/util.hpp`](../src/util.hpp)
- `parse_depth_update(std::string_view data)` in [`src/depth_update.hpp`](../src/depth_update.hpp)
- Internally in `parse_scaled_decimal` for `int_part` and `frac_part` substrings ([`src/util.cpp`](../src/util.cpp))

**Why:** The caller (`Session::on_read` in [`src/session.cpp`](../src/session.cpp)) already has the message as a `std::string`. Passing it to the parser as `string_view` avoids a second copy. The parser also splits the decimal string at the dot using `substr`, which for `string_view` just adjusts the pointer and length — no allocation.

**How:** `string_view` is implicitly constructible from `std::string`, so calling `parse_depth_update(data)` where `data` is a `std::string` works without conversion syntax. Inside `parse_scaled_decimal`:

```cpp
auto dot = s.find('.');
std::string_view int_part = s.substr(0, dot);   // no allocation
std::string_view frac_part = s.substr(dot + 1);  // no allocation
```

Each `substr` creates a new view pointing into the same original memory. Combined with `std::from_chars` (which takes raw pointers), the entire decimal-to-integer conversion involves zero heap allocations.

**Pitfalls:**
- `string_view` does not own the data. If the original string is destroyed or modified, the view becomes a dangling pointer. In our code this is safe: `data` (the `std::string`) outlives all `string_view`s derived from it within the same `on_read` call.
- `string_view::substr` does not null-terminate. Functions that require null-terminated input (like `strtol`) can't use it directly. `from_chars` accepts a `[begin, end)` range, which is why it pairs well with `string_view`.
