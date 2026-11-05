#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class HttpClient : public std::enable_shared_from_this<HttpClient> {
  public:
    struct Response {
        unsigned status;
        std::string body;
    };

    using Result = std::variant<Response, std::string>;
    using Callback = std::function<void(Result)>;

    HttpClient(net::io_context& ioc, ssl::context& ctx,
               std::string host, std::string port,
               std::chrono::steady_clock::duration timeout =
                   std::chrono::seconds(10));

    void async_get(std::string target, Callback cb);
    void cancel();

  private:
    static constexpr std::size_t kMaxBodySize = 2 * 1024 * 1024; // 2 MB

    net::io_context& ioc_;
    ssl::context& ssl_ctx_;
    tcp::resolver resolver_;
    std::optional<ssl::stream<beast::tcp_stream>> stream_;
    beast::flat_buffer buffer_;
    http::request<http::empty_body> req_;
    std::optional<http::response_parser<http::string_body>> res_parser_;
    net::steady_timer deadline_;

    std::string host_;
    std::string port_;
    std::chrono::steady_clock::duration timeout_;

    bool connected_ = false;
    bool busy_ = false;
    bool cancelling_ = false;
    bool timed_out_ = false;
    Callback cb_;

    void complete(Result result);
    void close_connection();
    void do_resolve();
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec,
                    tcp::resolver::results_type::endpoint_type ep);
    void on_tls_handshake(beast::error_code ec);
    void do_request();
    void on_write(beast::error_code ec, std::size_t bytes);
    void on_read(beast::error_code ec, std::size_t bytes);
    void on_deadline(beast::error_code ec);
};
