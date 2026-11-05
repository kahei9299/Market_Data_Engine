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
               std::string connect_host, std::string connect_port,
               std::string tls_host,
               std::chrono::steady_clock::duration timeout =
                   std::chrono::seconds(10));

    HttpClient(net::io_context& ioc, ssl::context& ctx,
               std::string host, std::string port,
               std::chrono::steady_clock::duration timeout =
                   std::chrono::seconds(10));

    void async_get(std::string target, Callback cb);
    void cancel();
    void abort();

  private:
    static constexpr std::size_t kMaxBodySize = 2 * 1024 * 1024; // 2 MB

    // Per-request async resources. Captured by callbacks via shared_ptr
    // so the stream/buffer/parser stay alive until all operations complete,
    // even after abort() drops the HttpClient's reference.
    struct Conn {
        ssl::stream<beast::tcp_stream> stream;
        beast::flat_buffer buffer;
        http::request<http::empty_body> req;
        std::optional<http::response_parser<http::string_body>> parser;
        net::steady_timer deadline;
        bool connected = false;

        Conn(net::io_context& ioc, ssl::context& ctx)
            : stream(ioc, ctx), deadline(ioc) {}
    };

    net::io_context& ioc_;
    ssl::context& ssl_ctx_;
    tcp::resolver resolver_;
    std::shared_ptr<Conn> conn_;

    std::string connect_host_;
    std::string connect_port_;
    std::string tls_host_;
    std::chrono::steady_clock::duration timeout_;

    bool busy_ = false;
    bool cancelling_ = false;
    bool timed_out_ = false;
    uint64_t request_gen_ = 0;
    Callback cb_;

    void complete(Result result, uint64_t gen);
    void do_resolve(std::shared_ptr<Conn> c, uint64_t gen);
    void on_resolve(std::shared_ptr<Conn> c, uint64_t gen,
                    beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(std::shared_ptr<Conn> c, uint64_t gen,
                    beast::error_code ec,
                    tcp::resolver::results_type::endpoint_type ep);
    void on_tls_handshake(std::shared_ptr<Conn> c, uint64_t gen,
                          beast::error_code ec);
    void do_request(std::shared_ptr<Conn> c, uint64_t gen);
    void on_write(std::shared_ptr<Conn> c, uint64_t gen,
                  beast::error_code ec, std::size_t bytes);
    void on_read(std::shared_ptr<Conn> c, uint64_t gen,
                 beast::error_code ec, std::size_t bytes);
    void on_deadline(std::shared_ptr<Conn> c, uint64_t gen,
                     beast::error_code ec);
};
