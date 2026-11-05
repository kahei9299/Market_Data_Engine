#pragma once

#include "depth_update.hpp"
#include "order_book.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct ReplayStats {
    uint64_t messages_parsed = 0;
    uint64_t messages_applied = 0;
    uint64_t messages_stale = 0;
    uint64_t messages_gap = 0;
    uint64_t parse_errors = 0;
    bool complete = false; // true only if all messages processed without gaps
};

struct ReplayResult {
    OrderBook book;
    ReplayStats stats;
};

// Load depth-update JSON messages from a JSONL file (one JSON per line).
// Returns empty vector on file error.
std::vector<std::string> load_messages(std::string const& path);

// Parse a vector of raw JSON strings into DepthUpdates.
// Malformed messages are counted in stats.parse_errors and skipped.
std::vector<DepthUpdate> parse_messages(std::vector<std::string> const& raw,
                                        ReplayStats& stats);

// Replay parsed updates through an OrderBook sequentially.
// The book must be initialized (e.g., via apply_snapshot) before calling.
ReplayResult replay_sequential(std::vector<DepthUpdate> const& updates,
                               uint64_t initial_update_id = 0);
