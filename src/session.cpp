#include "session.hpp"
#include "util.hpp"
#include "validation.hpp"

#include <boost/asio/connect.hpp>

#include <iostream>

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

void Session::run() {
    resolver_.async_resolve(
        host_, port_,
        beast::bind_front_handler(&Session::on_resolve, shared_from_this()));
}

void Session::stop() {
    if (stopping_)
        return;
    stopping_ = true;
    print_stats();

    resolver_.cancel();

    if (ws_.is_open()) {
        // WebSocket established — send a clean Close frame.
        ws_.async_close(
            websocket::close_code::normal,
            beast::bind_front_handler(&Session::on_close, shared_from_this()));
    } else {
        // Still in DNS/TCP/TLS/WS handshake — close the underlying socket.
        // Pending async operations get operation_aborted, which fail() suppresses.
        beast::error_code ec;
        beast::get_lowest_layer(ws_).socket().close(ec);
    }
}

void Session::fail(beast::error_code ec, char const* what) {
    if (ec == net::error::operation_aborted || stopping_)
        return;
    std::cerr << what << ": " << ec.message() << '\n';
    if (on_error_)
        on_error_(ec, what);
}

void Session::on_resolve(beast::error_code ec,
                         tcp::resolver::results_type results) {
    if (ec)
        return fail(ec, "resolve");
    if (stopping_)
        return;

    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(10));

    beast::get_lowest_layer(ws_).async_connect(
        results,
        beast::bind_front_handler(&Session::on_connect, shared_from_this()));
}

void Session::on_connect(beast::error_code ec,
                         tcp::resolver::results_type::endpoint_type) {
    if (ec)
        return fail(ec, "connect");
    if (stopping_)
        return;

    auto& ssl_stream = ws_.next_layer();

    // SNI: tell the server which hostname we want (for multi-domain hosts).
    if (!SSL_set_tlsext_host_name(ssl_stream.native_handle(), host_.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                               net::error::get_ssl_category());
        return fail(ec, "ssl_set_host_name");
    }

    // Certificate hostname verification: OpenSSL checks the certificate's
    // SAN/CN against this hostname during the handshake. Without this,
    // verify_peer only checks the CA chain, not whether the cert belongs
    // to the host we intended to connect to.
    if (!SSL_set1_host(ssl_stream.native_handle(), host_.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                               net::error::get_ssl_category());
        return fail(ec, "ssl_set1_host");
    }

    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(10));

    ssl_stream.async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&Session::on_ssl_handshake,
                                  shared_from_this()));
}

void Session::on_ssl_handshake(beast::error_code ec) {
    if (ec)
        return fail(ec, "ssl_handshake");
    if (stopping_)
        return;

    // Disable the tcp_stream timeout — Beast's WebSocket layer manages
    // its own idle timeout and ping/pong handling.
    beast::get_lowest_layer(ws_).expires_never();

    ws_.set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::client));

    ws_.set_option(
        websocket::stream_base::decorator([](websocket::request_type& req) {
            req.set(beast::http::field::user_agent, "market-data-engine/0.1");
        }));

    ws_.async_handshake(
        host_, path_,
        beast::bind_front_handler(&Session::on_handshake, shared_from_this()));
}

void Session::on_handshake(beast::error_code ec) {
    if (ec)
        return fail(ec, "handshake");
    if (stopping_)
        return;

    stats_.connected_at = std::chrono::steady_clock::now();
    std::cerr << "connected to " << host_ << path_ << '\n';
    do_read();
}

void Session::do_read() {
    if (stopping_)
        return;
    ws_.async_read(
        buffer_,
        beast::bind_front_handler(&Session::on_read, shared_from_this()));
}

void Session::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec)
        return fail(ec, "read");
    if (stopping_)
        return;

    auto data = beast::buffers_to_string(buffer_.data());

    stats_.messages_received++;
    stats_.bytes_received += bytes_transferred;

    auto result = validate_depth_message(data);
    if (!result.ok) {
        stats_.invalid_messages++;
        auto preview = data.substr(0, 120);
        std::cerr << "invalid message: " << result.error << " | " << preview
                  << '\n';
    }

    std::cout << '[' << timestamp_now() << "] " << data << '\n';

    buffer_.consume(bytes_transferred);
    do_read();
}

void Session::on_close(beast::error_code ec) {
    if (ec && ec != websocket::error::closed)
        fail(ec, "close");
}

void Session::print_stats() {
    std::cerr << "--- session stats ---\n";

    if (stats_.connected_at == std::chrono::steady_clock::time_point{}) {
        std::cerr << "  status:           never connected\n";
    } else {
        auto elapsed = std::chrono::steady_clock::now() - stats_.connected_at;
        auto secs =
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        std::cerr << "  uptime:           " << secs << "s\n";
    }

    std::cerr << "  messages:         " << stats_.messages_received << '\n'
              << "  bytes:            " << stats_.bytes_received << '\n'
              << "  invalid messages: " << stats_.invalid_messages << '\n';
}
