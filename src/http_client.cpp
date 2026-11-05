#include "http_client.hpp"

#include <boost/asio/connect.hpp>

#include <openssl/ssl.h>

HttpClient::HttpClient(net::io_context& ioc, ssl::context& ctx,
                       std::string host, std::string port,
                       std::chrono::steady_clock::duration timeout)
    : ioc_(ioc),
      ssl_ctx_(ctx),
      resolver_(ioc),
      stream_(std::in_place, ioc, ctx),
      deadline_(ioc),
      host_(std::move(host)),
      port_(std::move(port)),
      timeout_(timeout) {}

void HttpClient::async_get(std::string target, Callback cb) {
    if (busy_) {
        cb(std::string("request already in flight"));
        return;
    }

    busy_ = true;
    cancelling_ = false;
    timed_out_ = false;
    cb_ = std::move(cb);

    req_ = {};
    req_.method(http::verb::get);
    req_.target(target);
    req_.set(http::field::host, host_);
    req_.set(http::field::user_agent, "market-data-engine/0.2");
    req_.version(11);

    buffer_.consume(buffer_.size());
    res_parser_.emplace();
    res_parser_->body_limit(kMaxBodySize);

    deadline_.expires_after(timeout_);
    deadline_.async_wait(
        beast::bind_front_handler(&HttpClient::on_deadline,
                                  shared_from_this()));

    if (connected_) {
        do_request();
    } else {
        do_resolve();
    }
}

void HttpClient::cancel() {
    if (cancelling_)
        return;
    cancelling_ = true;
    deadline_.cancel();
    resolver_.cancel();
    close_connection();
}

void HttpClient::complete(Result result) {
    if (!busy_)
        return;
    deadline_.cancel();
    busy_ = false;
    if (cb_) {
        auto cb = std::move(cb_);
        cb_ = nullptr;
        cb(std::move(result));
    }
}

void HttpClient::close_connection() {
    if (stream_) {
        beast::error_code ec;
        beast::get_lowest_layer(*stream_).socket().close(ec);
    }
    connected_ = false;
    // Reconstruct stream to get fresh TLS session state.
    stream_.emplace(ioc_, ssl_ctx_);
}

void HttpClient::do_resolve() {
    resolver_.async_resolve(
        host_, port_,
        beast::bind_front_handler(&HttpClient::on_resolve,
                                  shared_from_this()));
}

void HttpClient::on_resolve(beast::error_code ec,
                            tcp::resolver::results_type results) {
    if (cancelling_)
        return complete(std::string("cancelled"));
    if (ec)
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("resolve: " + ec.message()));

    beast::get_lowest_layer(*stream_).async_connect(
        results,
        beast::bind_front_handler(&HttpClient::on_connect,
                                  shared_from_this()));
}

void HttpClient::on_connect(beast::error_code ec,
                            tcp::resolver::results_type::endpoint_type) {
    if (cancelling_)
        return complete(std::string("cancelled"));
    if (ec)
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("connect: " + ec.message()));

    auto* ssl_handle = stream_->native_handle();
    if (!SSL_set_tlsext_host_name(ssl_handle, host_.c_str())) {
        close_connection();
        return complete(std::string("ssl_set_host_name failed"));
    }
    if (!SSL_set1_host(ssl_handle, host_.c_str())) {
        close_connection();
        return complete(std::string("ssl_set1_host failed"));
    }

    stream_->async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&HttpClient::on_tls_handshake,
                                  shared_from_this()));
}

void HttpClient::on_tls_handshake(beast::error_code ec) {
    if (cancelling_)
        return complete(std::string("cancelled"));
    if (ec) {
        close_connection();
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("tls_handshake: " + ec.message()));
    }

    connected_ = true;
    do_request();
}

void HttpClient::do_request() {
    http::async_write(
        *stream_, req_,
        beast::bind_front_handler(&HttpClient::on_write,
                                  shared_from_this()));
}

void HttpClient::on_write(beast::error_code ec, std::size_t) {
    if (cancelling_)
        return complete(std::string("cancelled"));
    if (ec) {
        close_connection();
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("write: " + ec.message()));
    }

    http::async_read(
        *stream_, buffer_, *res_parser_,
        beast::bind_front_handler(&HttpClient::on_read,
                                  shared_from_this()));
}

void HttpClient::on_read(beast::error_code ec, std::size_t) {
    if (cancelling_)
        return complete(std::string("cancelled"));
    if (ec) {
        close_connection();
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("read: " + ec.message()));
    }

    auto& msg = res_parser_->get();

    if (msg.need_eof() || !msg.keep_alive())
        close_connection();

    complete(Response{msg.result_int(), std::move(msg.body())});
}

void HttpClient::on_deadline(beast::error_code ec) {
    if (ec == net::error::operation_aborted)
        return;
    timed_out_ = true;
    resolver_.cancel();
    close_connection();
}
