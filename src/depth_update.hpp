#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct PriceLevel {
    int64_t price;    // scaled by kScaleFactor (10^8), must be > 0
    int64_t quantity; // scaled by kScaleFactor (10^8), must be >= 0
};

struct DepthUpdate {
    std::string symbol;
    uint64_t event_time;      // exchange timestamp, milliseconds since epoch
    uint64_t first_update_id; // U
    uint64_t last_update_id;  // u
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};

struct ParseError {
    std::string message;
};

using ParseResult = std::variant<DepthUpdate, ParseError>;

// Parse a raw Binance depthUpdate JSON message into a typed DepthUpdate.
// Returns ParseError on any validation failure — never a partial DepthUpdate.
ParseResult parse_depth_update(std::string_view data);
