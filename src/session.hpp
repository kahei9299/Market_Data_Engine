#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

static constexpr std::size_t kMaxBufferSize = 1 * 1024 * 1024; // 1 MB

using ErrorCallback = std::function<void(beast::error_code, char const*)>;

struct SessionStats {
    std::uint64_t messages_received = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t invalid_messages = 0;
    std::chrono::steady_clock::time_point connected_at{};
};

// Owns the full async lifecycle of one WebSocket connection.
// Uses shared_from_this so that each async callback keeps the session alive
// until the operation completes — preventing use-after-free when the
// io_context outlives a stack-allocated session.
class Session : public std::enable_shared_from_this<Session> {
    tcp::resolver resolver_;
    websocket::stream<ssl::stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    bool stopping_ = false;

    std::string host_;
    std::string port_;
    std::string path_;

    ErrorCallback on_error_;
    SessionStats stats_;

  public:
    Session(net::io_context& ioc, ssl::context& ctx,
            std::string host, std::string port, std::string path,
            ErrorCallback on_error = {});

    void run();
    void stop();

    SessionStats const& stats() const { return stats_; }

  private:
    void fail(beast::error_code ec, char const* what);
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec,
                    tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_close(beast::error_code ec);
    void print_stats();
};
