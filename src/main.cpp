#include "session.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>

static constexpr auto kHost = "stream.binance.com";
static constexpr auto kPort = "9443";
static constexpr auto kPath = "/ws/btcusdt@depth";

int main() {
    try {
        net::io_context ioc;

        // Verify the server's TLS certificate against system CA roots.
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        auto session = std::make_shared<Session>(
            ioc, ctx, kHost, kPort, kPath,
            [](beast::error_code ec, char const* what) {
                std::cerr << "fatal: " << what << ": " << ec.message() << '\n';
            });
        session->run();

        // Ctrl-C triggers a clean WebSocket close instead of a raw kill.
        net::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&session](beast::error_code, int sig) {
            std::cerr << "\nreceived signal " << sig << ", shutting down...\n";
            session->stop();
        });

        ioc.run();

    } catch (std::exception const& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
