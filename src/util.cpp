#include "util.hpp"

#include <charconv>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>

std::string timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
        << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

ScaledResult parse_scaled_decimal(std::string_view s) {
    if (s.empty())
        return ScaledParseError{"empty input"};

    // Reject negative values including "-0" — from_chars parses "-0" as 0,
    // which would bypass the integer < 0 check.
    if (s[0] == '-')
        return ScaledParseError{"negative value"};

    auto dot = s.find('.');
    std::string_view int_part;
    std::string_view frac_part;

    if (dot == std::string_view::npos) {
        int_part = s;
    } else {
        int_part = s.substr(0, dot);
        frac_part = s.substr(dot + 1);

        if (frac_part.empty())
            return ScaledParseError{"trailing dot with no fractional digits"};

        if (static_cast<int>(frac_part.size()) > kDecimalScale)
            return ScaledParseError{
                "exceeds " + std::to_string(kDecimalScale) + " decimal places"};
    }

    if (int_part.empty())
        return ScaledParseError{"missing integer part"};

    // Parse integer part.
    int64_t integer = 0;
    auto [p1, ec1] = std::from_chars(int_part.data(),
                                      int_part.data() + int_part.size(),
                                      integer);
    if (ec1 != std::errc{} || p1 != int_part.data() + int_part.size())
        return ScaledParseError{"invalid integer part"};

    // Check overflow before scaling: integer * kScaleFactor must fit int64_t.
    static constexpr int64_t kMaxInteger =
        std::numeric_limits<int64_t>::max() / kScaleFactor;
    if (integer > kMaxInteger)
        return ScaledParseError{"integer part overflows scaled representation"};

    int64_t result = integer * kScaleFactor;

    // Parse fractional part.
    if (!frac_part.empty()) {
        int64_t frac = 0;
        auto [p2, ec2] = std::from_chars(frac_part.data(),
                                          frac_part.data() + frac_part.size(),
                                          frac);
        if (ec2 != std::errc{} || p2 != frac_part.data() + frac_part.size())
            return ScaledParseError{"invalid fractional part"};

        // Scale fractional part up to kDecimalScale digits.
        // e.g. frac="06" with kDecimalScale=8 → 06 * 10^6 = 6000000
        int missing_digits = kDecimalScale - static_cast<int>(frac_part.size());
        for (int i = 0; i < missing_digits; ++i)
            frac *= 10;

        // Check overflow before fractional addition.
        if (frac > std::numeric_limits<int64_t>::max() - result)
            return ScaledParseError{"overflows scaled representation"};

        result += frac;
    }

    return result;
}
