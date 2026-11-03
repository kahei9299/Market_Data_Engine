#pragma once

#include <string>
#include <string_view>

struct ValidationResult {
    bool ok = false;
    std::string error;
};

// Structural validation only: checks that a raw message is valid JSON with
// the top-level fields expected in a Binance depthUpdate (e, E, U, u, b, a).
// Does NOT validate bid/ask array contents, price/quantity formats, or
// sequence ID ranges.
ValidationResult validate_depth_message(std::string_view data);
