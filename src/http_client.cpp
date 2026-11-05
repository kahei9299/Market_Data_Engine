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
      conn_(std::make_shared<Conn>(ioc, ctx)),
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

    // Create fresh Conn if previous was closed or aborted.
    if (!conn_ || !conn_->connected)
        conn_ = std::make_shared<Conn>(ioc_, ssl_ctx_);

    auto c = conn_;
    c->req = {};
    c->req.method(http::verb::get);
    c->req.target(target);
    c->req.set(http::field::host, tls_host_);
    c->req.set(http::field::user_agent, "market-data-engine/0.2");
    c->req.version(11);

    c->buffer.consume(c->buffer.size());
    c->parser.emplace();
    c->parser->body_limit(kMaxBodySize);

    c->deadline.expires_after(timeout_);
    c->deadline.async_wait(
        [self = shared_from_this(), c, gen](beast::error_code ec) {
            self->on_deadline(c, gen, ec);
        });

    if (c->connected) {
        do_request(c, gen);
    } else {
        do_resolve(c, gen);
    }
}

void HttpClient::cancel() {
    if (cancelling_)
        return;
    cancelling_ = true;
    if (conn_) {
        conn_->deadline.cancel();
        beast::error_code ec;
        beast::get_lowest_layer(conn_->stream).socket().close(ec);
        conn_->connected = false;
    }
    resolver_.cancel();
}

void HttpClient::abort() {
    cancel();
    ++request_gen_;
    busy_ = false;
    cb_ = nullptr;
    // Drop reference to old Conn. It stays alive in any pending callbacks
    // until they fire and see the stale gen.
    conn_.reset();
}

void HttpClient::complete(Result result, uint64_t gen) {
    if (!busy_ || gen != request_gen_)
        return;
    if (conn_)
        conn_->deadline.cancel();
    busy_ = false;
    if (cb_) {
        auto cb = std::move(cb_);
        cb_ = nullptr;
        cb(std::move(result));
    }
}

void HttpClient::do_resolve(std::shared_ptr<Conn> c, uint64_t gen) {
    resolver_.async_resolve(
        connect_host_, connect_port_,
        [self = shared_from_this(), c, gen](
            beast::error_code ec, tcp::resolver::results_type results) {
            self->on_resolve(c, gen, ec, std::move(results));
        });
}

void HttpClient::on_resolve(std::shared_ptr<Conn> c, uint64_t gen,
                            beast::error_code ec,
                            tcp::resolver::results_type results) {
    if (gen != request_gen_) return;
    if (cancelling_)
        return complete(std::string("cancelled"), gen);
    if (ec)
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("resolve: " + ec.message()),
                        gen);

    beast::get_lowest_layer(c->stream).async_connect(
        results,
        [self = shared_from_this(), c, gen](
            beast::error_code ec,
            tcp::resolver::results_type::endpoint_type ep) {
            self->on_connect(c, gen, ec, ep);
        });
}

void HttpClient::on_connect(std::shared_ptr<Conn> c, uint64_t gen,
                            beast::error_code ec,
                            tcp::resolver::results_type::endpoint_type) {
    if (gen != request_gen_) return;
    if (cancelling_)
        return complete(std::string("cancelled"), gen);
    if (ec)
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("connect: " + ec.message()),
                        gen);

    auto* ssl_handle = c->stream.native_handle();
    if (!SSL_set_tlsext_host_name(ssl_handle, tls_host_.c_str())) {
        c->connected = false;
        return complete(std::string("ssl_set_host_name failed"), gen);
    }
    if (!SSL_set1_host(ssl_handle, tls_host_.c_str())) {
        c->connected = false;
        return complete(std::string("ssl_set1_host failed"), gen);
    }

    c->stream.async_handshake(
        ssl::stream_base::client,
        [self = shared_from_this(), c, gen](beast::error_code ec) {
            self->on_tls_handshake(c, gen, ec);
        });
}

void HttpClient::on_tls_handshake(std::shared_ptr<Conn> c, uint64_t gen,
                                  beast::error_code ec) {
    if (gen != request_gen_) return;
    if (cancelling_)
        return complete(std::string("cancelled"), gen);
    if (ec) {
        c->connected = false;
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("tls_handshake: " + ec.message()),
                        gen);
    }

    c->connected = true;
    do_request(c, gen);
}

void HttpClient::do_request(std::shared_ptr<Conn> c, uint64_t gen) {
    http::async_write(
        c->stream, c->req,
        [self = shared_from_this(), c, gen](
            beast::error_code ec, std::size_t bytes) {
            self->on_write(c, gen, ec, bytes);
        });
}

void HttpClient::on_write(std::shared_ptr<Conn> c, uint64_t gen,
                          beast::error_code ec, std::size_t) {
    if (gen != request_gen_) return;
    if (cancelling_)
        return complete(std::string("cancelled"), gen);
    if (ec) {
        c->connected = false;
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("write: " + ec.message()),
                        gen);
    }

    http::async_read(
        c->stream, c->buffer, *c->parser,
        [self = shared_from_this(), c, gen](
            beast::error_code ec, std::size_t bytes) {
            self->on_read(c, gen, ec, bytes);
        });
}

void HttpClient::on_read(std::shared_ptr<Conn> c, uint64_t gen,
                         beast::error_code ec, std::size_t) {
    if (gen != request_gen_) return;
    if (cancelling_)
        return complete(std::string("cancelled"), gen);
    if (ec) {
        c->connected = false;
        return complete(timed_out_ ? std::string("timeout")
                                   : std::string("read: " + ec.message()),
                        gen);
    }

    auto& msg = c->parser->get();

    if (msg.need_eof() || !msg.keep_alive())
        c->connected = false;

    complete(Response{msg.result_int(), std::move(msg.body())}, gen);
}

void HttpClient::on_deadline(std::shared_ptr<Conn> c, uint64_t gen,
                             beast::error_code ec) {
    if (ec == net::error::operation_aborted || gen != request_gen_)
        return;
    timed_out_ = true;
    resolver_.cancel();
    beast::error_code close_ec;
    beast::get_lowest_layer(c->stream).socket().close(close_ec);
    c->connected = false;
}
