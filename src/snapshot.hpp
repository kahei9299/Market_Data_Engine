#pragma once

#include "depth_update.hpp"

#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

struct Snapshot {
    uint64_t last_update_id;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};

using SnapshotResult = std::variant<Snapshot, ParseError>;

// Parse a Binance REST /api/v3/depth JSON response into a typed Snapshot.
// Returns ParseError on any validation failure — never a partial Snapshot.
// Skips zero-quantity entries (they carry no book state).
SnapshotResult parse_snapshot(std::string_view data);
