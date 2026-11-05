#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

std::string timestamp_now();

static constexpr int kDecimalScale = 8;
static constexpr int64_t kScaleFactor = 100'000'000; // 10^8

struct ScaledParseError {
    std::string message;
};

using ScaledResult = std::variant<int64_t, ScaledParseError>;

// Parse a decimal string (e.g. "78450.06000000") into a scaled int64_t.
// Rejects: more than kDecimalScale fractional digits, overflow, empty input,
// non-numeric characters. Does not go through floating-point.
ScaledResult parse_scaled_decimal(std::string_view s);

// Convert a scaled int64_t back to a decimal string with trimmed trailing zeros.
// e.g. 7845006000000 → "78450.06", 100000000 → "1"
std::string format_scaled(int64_t value);
