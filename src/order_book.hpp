#pragma once

#include "depth_update.hpp"
#include "snapshot.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <utility>

enum class UpdateResult {
    Applied, // Update applied successfully.
    Stale,   // Update ignored — already behind the book's update ID.
    Gap,     // Missed updates — U > update_id + 1. Book is stale.
};

class OrderBook {
    std::map<int64_t, int64_t> bids_; // price → quantity, descending iteration via rbegin
    std::map<int64_t, int64_t> asks_; // price → quantity, ascending iteration via begin
    uint64_t update_id_ = 0;

  public:
    // Replace the entire book with snapshot contents.
    void apply_snapshot(Snapshot const& snap);

    // Apply a differential depth update per the Binance update procedure.
    // Returns Applied, Stale, or Gap.
    UpdateResult apply_update(DepthUpdate const& update);

    // Highest bid: price and quantity. Empty if no bids.
    std::optional<std::pair<int64_t, int64_t>> best_bid() const;

    // Lowest ask: price and quantity. Empty if no asks.
    std::optional<std::pair<int64_t, int64_t>> best_ask() const;

    uint64_t update_id() const { return update_id_; }
    std::size_t bid_count() const { return bids_.size(); }
    std::size_t ask_count() const { return asks_.size(); }
};
