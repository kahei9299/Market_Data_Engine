#pragma once

#include "depth_update.hpp"
#include "http_client.hpp"
#include "order_book.hpp"
#include "snapshot.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>

enum class SyncState {
    Buffering,       // WS connected, queuing events, waiting for first event.
    SnapshotPending, // Snapshot request in flight, still buffering.
    Synchronized,    // Book is valid, applying updates directly.
};

enum class SyncEvent {
    Applied,        // Update applied, book changed.
    Stale,          // Update ignored (behind book's update ID).
    Buffered,       // Update buffered (not yet synchronized).
    Synchronized,   // Book just became synchronized.
    Resyncing,      // Gap or error detected, restarting sync.
};

class SyncEngine : public std::enable_shared_from_this<SyncEngine> {
  public:
    static constexpr std::size_t kMaxBufferSize = 1000;
    static constexpr int kMaxSnapshotRetries = 5;

    struct Config {
        std::string symbol = "BTCUSDT";
        int snapshot_limit = 1000;
        std::string snapshot_host = "api.binance.com";
        std::string snapshot_port = "443";
        std::string snapshot_connect_host;
        std::chrono::steady_clock::duration http_timeout =
            std::chrono::seconds(10);
        std::chrono::milliseconds base_retry_delay{1000};

        // If empty, snapshot_connect_host defaults to snapshot_host.
        std::string effective_connect_host() const {
            return snapshot_connect_host.empty() ? snapshot_host
                                                 : snapshot_connect_host;
        }
    };

    SyncEngine(net::io_context& ioc, ssl::context& ssl_ctx, Config config);

    SyncEvent on_update(DepthUpdate const& update);
    OrderBook const& book() const { return book_; }
    SyncState state() const { return state_; }

    void stop();

  private:
    net::io_context& ioc_;
    Config config_;
    OrderBook book_;
    std::deque<DepthUpdate> buffer_;
    SyncState state_ = SyncState::Buffering;
    uint64_t first_buffered_U_ = 0;
    bool have_first_event_ = false;
    int snapshot_retries_ = 0;
    uint64_t sync_gen_ = 0;
    std::shared_ptr<HttpClient> http_client_;
    net::steady_timer retry_timer_;
    bool stopping_ = false;

    void request_snapshot();
    void on_snapshot_response(uint64_t gen, HttpClient::Result result);
    void apply_snapshot_and_sync(Snapshot const& snap);
    void begin_resync();

    std::string snapshot_target() const;
};
