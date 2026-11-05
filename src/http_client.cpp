#include "http_client.hpp"

#include <boost/asio/connect.hpp>

#include <openssl/ssl.h>

HttpClient::HttpClient(net::io_context& ioc, ssl::context& ctx,
                       std::string connect_host, std::string connect_port,
                       std::string tls_host,
                       std::chrono::steady_clock::duration timeout)
    : ioc_(ioc),
      ssl_ctx_(ctx),
      resolver_(ioc),
      stream_(std::in_place, ioc, ctx),
      deadline_(ioc),
      connect_host_(std::move(connect_host)),
      connect_port_(std::move(connect_port)),
      tls_host_(std::move(tls_host)),
      timeout_(timeout) {}

HttpClient::HttpClient(net::io_context& ioc, ssl::context& ctx,
                       std::string host, std::string port,
                       std::chrono::steady_clock::duration timeout)
    : HttpClient(ioc, ctx, host, port, host, timeout) {}

void HttpClient::async_get(std::string target, Callback cb) {
    if (busy_) {
        cb(std::string("request already in flight"));
        return;
    }

    busy_ = true;
    cancelling_ = false;
    timed_out_ = false;
    auto gen = ++request_gen_;
    cb_ = std::move(cb);

    req_ = {};
    req_.method(http::verb::get);
    req_.target(target);
    req_.set(http::field::host, tls_host_);
    req_.set(http::field::user_agent, "market-data-engine/0.2");
    req_.version(11);

    buffer_.consume(buffer_.size());
    res_parser_.emplace();
    res_parser_->body_limit(kMaxBodySize);

    deadline_.expires_after(timeout_);
    deadline_.async_wait(
        [self = shared_from_this(), gen](beast::error_code ec) {
            self->on_deadline(gen, ec);
        });

    if (connected_) {
        do_request(gen);
    } else {
        ensure_fresh_stream();
        do_resolve(gen);
    }
}

void HttpClient::cancel() {
    if (cancelling_)
        return;
    cancelling_ = true;
    deadline_.cancel();
    resolver_.cancel();
    close_socket();
}

void HttpClient::abort() {
    cancel();
    // Invalidate the current request gen so stale callbacks are ignored,
    // then clear busy so a new request can be accepted immediately.
    ++request_gen_;
    busy_ = false;
    cb_ = nullptr;
}

void HttpClient::complete(Result result, uint64_t gen) {
    if (!busy_ || gen != request_gen_)
        return;
    deadline_.cancel();
    busy_ = false;
    if (cb_) {
        auto cb = std::move(cb_);
        cb_ = nullptr;
        cb(std::move(result));
    }
}

void HttpClient::close_socket() {
    if (stream_) {
        beast::error_code ec;
        beast::get_lowest_layer(*stream_).socket().close(ec);
    }
    connected_ = false;
}

void HttpClient::ensure_fresh_stream() {
    if (!connected_) {
        stream_.emplace(ioc_, ssl_ctx_);
    }
}

void HttpClient::do_resolve(uint64_t gen) {
    resolver_.async_resolve(
        connect_host_, connect_port_,
        [self = shared_from_this(), gen](beast::error_code ec,
                                         tcp::resolver::results_type results) {
            self->on_resolve(gen, ec, std::move(results));
        });
}

void HttpClient::on_resolve(uint64_t gen, beast::error_code ec,
                            tcp::resolver::results_type results) {
    if (gen != request_gen_) return;
    if (cancelling_)
        return complete(std::string("cancelled"), gen);
    if (ec)
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("resolve: " + ec.message()),
                        gen);

    beast::get_lowest_layer(*stream_).async_connect(
        results,
        [self = shared_from_this(), gen](beast::error_code ec,
                                         tcp::resolver::results_type::endpoint_type ep) {
            self->on_connect(gen, ec, ep);
        });
}

void HttpClient::on_connect(uint64_t gen, beast::error_code ec,
                            tcp::resolver::results_type::endpoint_type) {
    if (gen != request_gen_) return;
    if (cancelling_)
        return complete(std::string("cancelled"), gen);
    if (ec)
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("connect: " + ec.message()),
                        gen);

    auto* ssl_handle = stream_->native_handle();
    if (!SSL_set_tlsext_host_name(ssl_handle, tls_host_.c_str())) {
        close_socket();
        return complete(std::string("ssl_set_host_name failed"), gen);
    }
    if (!SSL_set1_host(ssl_handle, tls_host_.c_str())) {
        close_socket();
        return complete(std::string("ssl_set1_host failed"), gen);
    }

    stream_->async_handshake(
        ssl::stream_base::client,
        [self = shared_from_this(), gen](beast::error_code ec) {
            self->on_tls_handshake(gen, ec);
        });
}

void HttpClient::on_tls_handshake(uint64_t gen, beast::error_code ec) {
    if (gen != request_gen_) return;
    if (cancelling_)
        return complete(std::string("cancelled"), gen);
    if (ec) {
        close_socket();
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("tls_handshake: " + ec.message()),
                        gen);
    }

    connected_ = true;
    do_request(gen);
}

void HttpClient::do_request(uint64_t gen) {
    http::async_write(
        *stream_, req_,
        [self = shared_from_this(), gen](beast::error_code ec, std::size_t bytes) {
            self->on_write(gen, ec, bytes);
        });
}

void HttpClient::on_write(uint64_t gen, beast::error_code ec, std::size_t) {
    if (gen != request_gen_) return;
    if (cancelling_)
        return complete(std::string("cancelled"), gen);
    if (ec) {
        close_socket();
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("write: " + ec.message()),
                        gen);
    }

    http::async_read(
        *stream_, buffer_, *res_parser_,
        [self = shared_from_this(), gen](beast::error_code ec, std::size_t bytes) {
            self->on_read(gen, ec, bytes);
        });
}

void HttpClient::on_read(uint64_t gen, beast::error_code ec, std::size_t) {
    if (gen != request_gen_) return;
    if (cancelling_)
        return complete(std::string("cancelled"), gen);
    if (ec) {
        close_socket();
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("read: " + ec.message()),
                        gen);
    }

    auto& msg = res_parser_->get();

    if (msg.need_eof() || !msg.keep_alive())
        close_socket();

    complete(Response{msg.result_int(), std::move(msg.body())}, gen);
}

void HttpClient::on_deadline(uint64_t gen, beast::error_code ec) {
    if (ec == net::error::operation_aborted || gen != request_gen_)
        return;
    timed_out_ = true;
    resolver_.cancel();
    close_socket();
}
