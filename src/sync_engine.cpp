#include "sync_engine.hpp"
#include "util.hpp"

#include <iostream>

SyncEngine::SyncEngine(net::io_context& ioc, ssl::context& ssl_ctx,
                       Config config)
    : ioc_(ioc),
      config_(std::move(config)),
      http_client_(std::make_shared<HttpClient>(
          ioc, ssl_ctx, config_.snapshot_host, config_.snapshot_port,
          config_.http_timeout)),
      retry_timer_(ioc) {}

SyncEvent SyncEngine::on_update(DepthUpdate const& update) {
    if (stopping_)
        return SyncEvent::Stale;

    switch (state_) {
    case SyncState::Buffering:
    case SyncState::SnapshotPending: {
        if (buffer_.size() >= kMaxBufferSize) {
            std::cerr << "[" << timestamp_now()
                      << "] sync: buffer overflow (" << kMaxBufferSize
                      << " events), resyncing\n";
            begin_resync();
            return SyncEvent::Resyncing;
        }

        buffer_.push_back(update);

        if (!have_first_event_) {
            have_first_event_ = true;
            first_buffered_U_ = update.first_update_id;
            std::cerr << "[" << timestamp_now()
                      << "] sync: first event received (U="
                      << first_buffered_U_ << "), requesting snapshot\n";
            request_snapshot();
        }

        return SyncEvent::Buffered;
    }

    case SyncState::Synchronized: {
        auto result = book_.apply_update(update);
        switch (result) {
        case UpdateResult::Applied:
            return SyncEvent::Applied;
        case UpdateResult::Stale:
            return SyncEvent::Stale;
        case UpdateResult::Gap:
            std::cerr << "[" << timestamp_now()
                      << "] sync: gap detected (expected U<="
                      << book_.update_id() + 1 << ", got U="
                      << update.first_update_id << "), resyncing\n";
            begin_resync();
            return SyncEvent::Resyncing;
        }
    }
    }
    return SyncEvent::Stale;
}

void SyncEngine::stop() {
    stopping_ = true;
    retry_timer_.cancel();
    if (http_client_)
        http_client_->cancel();
}

void SyncEngine::request_snapshot() {
    state_ = SyncState::SnapshotPending;

    http_client_->async_get(
        snapshot_target(),
        [this, self = http_client_](HttpClient::Result result) {
            on_snapshot_response(std::move(result));
        });
}

void SyncEngine::on_snapshot_response(HttpClient::Result result) {
    if (stopping_)
        return;

    // Transport error.
    if (auto* err = std::get_if<std::string>(&result)) {
        std::cerr << "[" << timestamp_now()
                  << "] sync: snapshot request failed: " << *err << "\n";
        begin_resync();
        return;
    }

    auto& resp = std::get<HttpClient::Response>(result);

    // HTTP error.
    if (resp.status != 200) {
        std::cerr << "[" << timestamp_now()
                  << "] sync: snapshot HTTP " << resp.status << "\n";
        begin_resync();
        return;
    }

    // Parse snapshot.
    auto snap_result = parse_snapshot(resp.body);
    if (auto* err = std::get_if<ParseError>(&snap_result)) {
        std::cerr << "[" << timestamp_now()
                  << "] sync: snapshot parse error: " << err->message << "\n";
        begin_resync();
        return;
    }

    auto& snap = std::get<Snapshot>(snap_result);

    // Step 4: If snapshot is too stale, retry.
    if (have_first_event_ && snap.last_update_id < first_buffered_U_) {
        snapshot_retries_++;
        if (snapshot_retries_ > kMaxSnapshotRetries) {
            std::cerr << "[" << timestamp_now()
                      << "] sync: snapshot retries exhausted, resyncing\n";
            begin_resync();
            return;
        }

        auto delay = config_.base_retry_delay *
                     (1 << (snapshot_retries_ - 1));
        std::cerr << "[" << timestamp_now()
                  << "] sync: snapshot stale (lastUpdateId="
                  << snap.last_update_id << " < first_U="
                  << first_buffered_U_ << "), retry " << snapshot_retries_
                  << "/" << kMaxSnapshotRetries << " in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         delay).count()
                  << "ms\n";

        retry_timer_.expires_after(delay);
        retry_timer_.async_wait([this](beast::error_code ec) {
            if (ec || stopping_)
                return;
            request_snapshot();
        });
        return;
    }

    apply_snapshot_and_sync(snap);
}

void SyncEngine::apply_snapshot_and_sync(Snapshot const& snap) {
    // Step 5: Discard buffered events where u <= lastUpdateId.
    while (!buffer_.empty() &&
           buffer_.front().last_update_id <= snap.last_update_id) {
        buffer_.pop_front();
    }

    // The first remaining event should have lastUpdateId within [U, u].
    if (!buffer_.empty()) {
        auto& first = buffer_.front();
        if (first.first_update_id > snap.last_update_id + 1) {
            std::cerr << "[" << timestamp_now()
                      << "] sync: no bridging event in buffer (first U="
                      << first.first_update_id << ", need <="
                      << snap.last_update_id + 1 << "), resyncing\n";
            begin_resync();
            return;
        }
    }

    // Step 6: Set local order book to snapshot.
    book_.apply_snapshot(snap);

    std::cerr << "[" << timestamp_now()
              << "] sync: snapshot applied (lastUpdateId="
              << snap.last_update_id << ", bids=" << book_.bid_count()
              << ", asks=" << book_.ask_count() << ")\n";

    // Step 7: Apply buffered events.
    state_ = SyncState::Synchronized;
    while (!buffer_.empty()) {
        auto& ev = buffer_.front();
        auto result = book_.apply_update(ev);
        if (result == UpdateResult::Gap) {
            std::cerr << "[" << timestamp_now()
                      << "] sync: gap in buffered events (expected U<="
                      << book_.update_id() + 1 << ", got U="
                      << ev.first_update_id << "), resyncing\n";
            begin_resync();
            return;
        }
        buffer_.pop_front();
    }

    std::cerr << "[" << timestamp_now()
              << "] sync: book synchronized (update_id="
              << book_.update_id() << ", bids=" << book_.bid_count()
              << ", asks=" << book_.ask_count() << ")\n";
}

void SyncEngine::begin_resync() {
    book_ = OrderBook{};
    buffer_.clear();
    have_first_event_ = false;
    first_buffered_U_ = 0;
    snapshot_retries_ = 0;
    state_ = SyncState::Buffering;
    retry_timer_.cancel();
}

std::string SyncEngine::snapshot_target() const {
    return "/api/v3/depth?symbol=" + config_.symbol +
           "&limit=" + std::to_string(config_.snapshot_limit);
}
