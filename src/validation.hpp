#pragma once

#include <string>
#include <string_view>

struct ValidationResult {
    bool ok = false;
    std::string error;
};

// Validates that a raw message is a well-formed Binance depthUpdate.
// Checks: valid JSON, event type "depthUpdate", numeric E/U/u fields,
// array b and a fields.
ValidationResult validate_depth_message(std::string_view data);
