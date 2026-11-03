# Learning Journal

C++ and systems engineering concepts encountered in the project, organized by milestone.

## M1.1 — Live WebSocket Receiver

### Asio's async event loop

The program is driven by a single `net::io_context`. Async operations (`async_resolve`, `async_connect`, `async_read`, etc.) register work with the OS and return immediately. `ioc.run()` blocks and dispatches completions one at a time.

Nothing happens until `ioc.run()` is called. Everything before it is setup — registering work. Everything after it is cleanup.

`ioc.run()` returns when there is no more pending work. This is why the `signal_set`'s `async_wait` matters — it keeps `run()` alive until a signal arrives. If a fatal error occurs, cancelling the signal set allows `run()` to return.

### The async callback chain

Each step in the connection sequence starts an async operation and provides the next step as a callback:

```
run() → on_resolve → on_connect → on_ssl_handshake → on_handshake → do_read ↔ on_read
```

This is not recursion. `do_read()` registers an async read and returns immediately. `on_read` is invoked later by `ioc.run()`. The call stack never grows.

### Object lifetimes and `shared_from_this`

**The problem:** Async callbacks hold references to `Session` members (socket, buffer). If `Session` is destroyed before those callbacks fire, the program accesses freed memory.

**The solution:** `Session` inherits from `std::enable_shared_from_this<Session>` and must be created via `std::make_shared<Session>(...)`. Each callback captures `shared_from_this()` — a `shared_ptr` that keeps the Session alive. When the last callback completes and releases its `shared_ptr`, the reference count drops to zero and Session is destroyed.

```cpp
// Each callback captures a shared_ptr to keep Session alive:
beast::bind_front_handler(&Session::on_read, shared_from_this())
```

**Critical rule:** Never create a `Session` on the stack. `shared_from_this()` will throw `std::bad_weak_ptr` at runtime because the internal `weak_ptr` is only initialized when a `shared_ptr` takes ownership.

### Layered stream architecture

Beast stacks protocol layers inside-out:

```cpp
websocket::stream<ssl::stream<beast::tcp_stream>> ws_;
//                                 ↑ innermost: TCP with timeout
//                  ↑ middle: TLS encryption
// ↑ outermost: WebSocket framing
```

`ws_.async_read()` reads a WebSocket frame → decrypts via TLS → reads TCP bytes. `ws_.next_layer()` peels one layer (WebSocket → SSL). `beast::get_lowest_layer(ws_)` reaches the TCP stream directly.

### Buffer ownership and `consume()`

`beast::flat_buffer` is a contiguous, resizable byte buffer owned by `Session`. It's reused across reads — no per-message allocation (except the `std::string` copy for printing).

**Critical:** `buffer_.consume(bytes_transferred)` must be called after every read. It marks the bytes as processed. Without it, the buffer accumulates all messages ever received and eventually hits the 1 MB limit.

```cpp
auto data = beast::buffers_to_string(buffer_.data());  // copy to string
buffer_.consume(bytes_transferred);                      // free the space
```

### TLS: SNI vs. hostname verification

**Learned during code review — this was a real bug.**

Two separate security mechanisms that are easy to confuse:

| Function | Purpose | What happens without it |
|---|---|---|
| `SSL_set_tlsext_host_name()` | **SNI** — tells the server which hostname the client wants | Multi-domain servers may serve the wrong certificate |
| `SSL_set1_host()` | **Hostname verification** — OpenSSL checks the cert's SAN/CN matches | Any valid certificate from a trusted CA is accepted, including one for `evil.com` |

The initial implementation only called `SSL_set_tlsext_host_name`, which configures SNI but does **not** verify the hostname. `verify_peer` validates the certificate chain (is it signed by a trusted CA?) but doesn't check which domain the certificate belongs to. Both `verify_peer` and `SSL_set1_host` are needed for correct TLS security.

### Async cancellation and shutdown races

**Learned during code review — a subtle concurrency issue even in single-threaded code.**

`stop()` cancels the resolver and closes the socket. Cancellation is a **request**, not a guarantee. It has three possible outcomes for each pending operation:

1. **The operation hasn't started yet** — it completes with `operation_aborted`.
2. **The operation is in progress** — closing the socket interrupts it; the callback fires with an error (which may or may not be `operation_aborted` — the specific error depends on the OS and the operation's state).
3. **The operation already completed successfully** — cancellation has no effect. The callback fires with no error and, without a guard, would proceed to start the next connection stage.

Case 3 is the dangerous one. It's a race: the resolve completes, its successful result is queued for dispatch, and then `stop()` runs. The callback will see `ec` with no error and happily call `async_connect` — potentially on a socket that `stop()` already closed.

**Fix:** Every callback checks `if (stopping_) return;` after the error check.

```cpp
void Session::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec)
        return fail(ec, "resolve");   // may be operation_aborted or another error
    if (stopping_)
        return;                        // completed successfully, but we're shutting down
    // ... proceed with connect
}
```

**Key insight:** In async code, cancellation means "try to stop pending work." It does not guarantee any particular error code, and it cannot recall a callback that's already queued for dispatch. Explicit state checks (`stopping_`) are the only reliable way to prevent post-shutdown operations.

### `error_code` vs. exceptions

Asio operations offer two styles:

```cpp
// Throwing — use for setup code where failure is fatal:
ssl::context ctx(ssl::context::tlsv12_client);  // throws on failure

// Non-throwing — use for async callbacks where errors are expected:
json::parse(data, ec);  // sets ec instead of throwing
if (ec) return {false, "invalid JSON"};
```

Network errors (disconnects, timeouts, DNS failures) are normal operation, not exceptional. Using `error_code` avoids the overhead of exception unwinding and makes error handling explicit at every call site.

### `std::move` and sink parameters

```cpp
Session::Session(..., std::string host, ...)
    : host_(std::move(host)) {}
```

The constructor takes `host` **by value**. The caller either copies (if they keep their string) or moves (if they don't). Inside the constructor, `std::move` casts `host` to an rvalue reference, enabling the move constructor of `host_` to steal the source's resources instead of copying. For heap-allocated strings, this transfers the internal buffer pointer. For short strings (typically ≤ 22 bytes), implementations use Small String Optimization (SSO) and store the characters inline — a "move" of a short string is actually a copy since there's no heap allocation to transfer.

After the move, `host` is in a **valid but unspecified** state. The standard does not guarantee it's empty — only that it can be safely destroyed or assigned to. Don't read a moved-from object.

This "take by value, then move" idiom gives the caller the choice of copy or move, with at most one copy total.

### Anonymous namespaces for file-private functions

When a helper function is only needed within a single `.cpp` file, an anonymous namespace restricts its visibility to that translation unit:

```cpp
// In some_file.cpp — not visible to any other .cpp file:
namespace {
int internal_helper() { return 42; }
}
```

This is the C++ equivalent of `static` for free functions. Other translation units can't call or collide with it. In our project, `timestamp_now()` was originally in an anonymous namespace inside `session.cpp` before being promoted to a shared function in `util.hpp` so tests could use it. The technique applies whenever a function is an implementation detail of a single file.

### `std::chrono` time handling

Two clocks serve different purposes:

| Clock | Use | Properties |
|---|---|---|
| `system_clock` | Wall-clock time (timestamps) | Can jump backward (NTP, manual adjustment) |
| `steady_clock` | Elapsed time (uptime) | Monotonic, never jumps |

`timestamp_now()` uses `system_clock` for human-readable output. `SessionStats::connected_at` uses `steady_clock` for reliable uptime measurement.

### CMake static libraries and link propagation

```cmake
add_library(engine_core STATIC src/session.cpp src/util.cpp src/validation.cpp)
target_link_libraries(engine_core PUBLIC Boost::system Boost::json OpenSSL::SSL)
```

`engine_core` is compiled once into `libengine_core.a`. Both the main executable and tests link against it — avoiding double compilation of shared sources.

`PUBLIC` vs `PRIVATE` on `target_link_libraries`: `PUBLIC` means dependents also get the library. The test executable links `engine_core` and automatically gets Boost and OpenSSL. `PRIVATE` means the dependency stops at that target.
