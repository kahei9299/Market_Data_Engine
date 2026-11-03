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

        // Declared before session so the error callback can capture it.
        net::signal_set signals(ioc, SIGINT, SIGTERM);
        int exit_code = EXIT_SUCCESS;

        auto session = std::make_shared<Session>(
            ioc, ctx, kHost, kPort, kPath,
            [&signals, &exit_code](beast::error_code, char const*) {
                exit_code = EXIT_FAILURE;
                signals.cancel();
            });
        session->run();

        signals.async_wait([&session](beast::error_code ec, int sig) {
            if (ec)
                return; // cancelled by error callback
            std::cerr << "\nreceived signal " << sig << ", shutting down...\n";
            session->stop();
        });

        ioc.run();
        return exit_code;

    } catch (std::exception const& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
