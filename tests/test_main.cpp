#define BOOST_TEST_MODULE market_data_engine_tests
#include <boost/test/unit_test.hpp>

#include "depth_update.hpp"
#include "order_book.hpp"
#include "session.hpp"
#include "snapshot.hpp"
#include "util.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>
#include <string>

// --- timestamp_now tests (M1.1) ---

BOOST_AUTO_TEST_SUITE(timestamp)

BOOST_AUTO_TEST_CASE(format_matches_iso8601) {
    auto ts = timestamp_now();
    std::regex pattern(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)");
    BOOST_TEST(std::regex_match(ts, pattern));
}

BOOST_AUTO_TEST_CASE(length_is_24) {
    auto ts = timestamp_now();
    BOOST_TEST(ts.size() == 24u);
}

BOOST_AUTO_TEST_SUITE_END()

// --- scaled decimal parsing (M1.2) ---

BOOST_AUTO_TEST_SUITE(scaled_decimal)

BOOST_AUTO_TEST_CASE(integer_only) {
    auto r = parse_scaled_decimal("12345");
    BOOST_REQUIRE(std::holds_alternative<int64_t>(r));
    BOOST_TEST(std::get<int64_t>(r) == 12345LL * kScaleFactor);
}

BOOST_AUTO_TEST_CASE(with_fractional) {
    auto r = parse_scaled_decimal("78450.06000000");
    BOOST_REQUIRE(std::holds_alternative<int64_t>(r));
    BOOST_TEST(std::get<int64_t>(r) == 7845006000000LL);
}

BOOST_AUTO_TEST_CASE(short_fractional_padded) {
    auto r = parse_scaled_decimal("1.5");
    BOOST_REQUIRE(std::holds_alternative<int64_t>(r));
    BOOST_TEST(std::get<int64_t>(r) == 150000000LL);
}

BOOST_AUTO_TEST_CASE(zero) {
    auto r = parse_scaled_decimal("0");
    BOOST_REQUIRE(std::holds_alternative<int64_t>(r));
    BOOST_TEST(std::get<int64_t>(r) == 0LL);
}

BOOST_AUTO_TEST_CASE(zero_with_decimals) {
    auto r = parse_scaled_decimal("0.00000000");
    BOOST_REQUIRE(std::holds_alternative<int64_t>(r));
    BOOST_TEST(std::get<int64_t>(r) == 0LL);
}

BOOST_AUTO_TEST_CASE(max_precision_8_digits) {
    auto r = parse_scaled_decimal("0.12345678");
    BOOST_REQUIRE(std::holds_alternative<int64_t>(r));
    BOOST_TEST(std::get<int64_t>(r) == 12345678LL);
}

BOOST_AUTO_TEST_CASE(exceeds_precision) {
    auto r = parse_scaled_decimal("0.123456789");
    BOOST_REQUIRE(std::holds_alternative<ScaledParseError>(r));
    BOOST_TEST(std::get<ScaledParseError>(r).message.find("decimal places") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(empty_input) {
    auto r = parse_scaled_decimal("");
    BOOST_TEST(std::holds_alternative<ScaledParseError>(r));
}

BOOST_AUTO_TEST_CASE(trailing_dot) {
    auto r = parse_scaled_decimal("123.");
    BOOST_TEST(std::holds_alternative<ScaledParseError>(r));
}

BOOST_AUTO_TEST_CASE(non_numeric) {
    auto r = parse_scaled_decimal("abc");
    BOOST_TEST(std::holds_alternative<ScaledParseError>(r));
}

BOOST_AUTO_TEST_CASE(negative_rejected) {
    auto r = parse_scaled_decimal("-1.0");
    BOOST_TEST(std::holds_alternative<ScaledParseError>(r));
}

BOOST_AUTO_TEST_CASE(negative_zero_rejected) {
    auto r = parse_scaled_decimal("-0");
    BOOST_TEST(std::holds_alternative<ScaledParseError>(r));
}

BOOST_AUTO_TEST_CASE(negative_zero_decimal_rejected) {
    auto r = parse_scaled_decimal("-0.00000000");
    BOOST_TEST(std::holds_alternative<ScaledParseError>(r));
}

BOOST_AUTO_TEST_CASE(overflow_large_integer) {
    auto r = parse_scaled_decimal("99999999999999999");
    BOOST_REQUIRE(std::holds_alternative<ScaledParseError>(r));
    BOOST_TEST(std::get<ScaledParseError>(r).message.find("overflow") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(boundary_max_safe_integer) {
    // max int64 / 10^8 = 92233720368
    auto r = parse_scaled_decimal("92233720368.0");
    BOOST_REQUIRE(std::holds_alternative<int64_t>(r));
    BOOST_TEST(std::get<int64_t>(r) == 92233720368LL * kScaleFactor);
}

BOOST_AUTO_TEST_CASE(boundary_just_over_max) {
    auto r = parse_scaled_decimal("92233720369.0");
    BOOST_TEST(std::holds_alternative<ScaledParseError>(r));
}

BOOST_AUTO_TEST_CASE(exact_int64_max) {
    // INT64_MAX = 9223372036854775807 = 92233720368.54775807 at scale 10^8
    auto r = parse_scaled_decimal("92233720368.54775807");
    BOOST_REQUIRE(std::holds_alternative<int64_t>(r));
    BOOST_TEST(std::get<int64_t>(r) == std::numeric_limits<int64_t>::max());
}

BOOST_AUTO_TEST_CASE(one_past_int64_max) {
    // 92233720368.54775808 = INT64_MAX + 1 — must be rejected
    auto r = parse_scaled_decimal("92233720368.54775808");
    BOOST_REQUIRE(std::holds_alternative<ScaledParseError>(r));
    BOOST_TEST(std::get<ScaledParseError>(r).message.find("overflow") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(max_integer_with_large_frac_overflows) {
    // 92233720368.99999999 — integer part fits but frac causes overflow
    auto r = parse_scaled_decimal("92233720368.99999999");
    BOOST_TEST(std::holds_alternative<ScaledParseError>(r));
}

BOOST_AUTO_TEST_SUITE_END()

// --- depth update parsing (M1.2) ---

BOOST_AUTO_TEST_SUITE(depth_update_parsing)

static const std::string kValidMessage = R"({
    "e": "depthUpdate",
    "E": 1793587539014,
    "s": "BTCUSDT",
    "U": 102171391360,
    "u": 102171391511,
    "b": [["78450.06000000","0.04581000"]],
    "a": [["78450.07000000","2.27294000"]]
})";

BOOST_AUTO_TEST_CASE(valid_message_parses) {
    auto r = parse_depth_update(kValidMessage);
    BOOST_REQUIRE(std::holds_alternative<DepthUpdate>(r));
    auto const& u = std::get<DepthUpdate>(r);
    BOOST_TEST(u.symbol == "BTCUSDT");
    BOOST_TEST(u.event_time == 1793587539014ULL);
    BOOST_TEST(u.first_update_id == 102171391360ULL);
    BOOST_TEST(u.last_update_id == 102171391511ULL);
    BOOST_TEST(u.bids.size() == 1u);
    BOOST_TEST(u.asks.size() == 1u);
    BOOST_TEST(u.bids[0].price == 7845006000000LL);
    BOOST_TEST(u.bids[0].quantity == 4581000LL);
    BOOST_TEST(u.asks[0].price == 7845007000000LL);
    BOOST_TEST(u.asks[0].quantity == 227294000LL);
}

BOOST_AUTO_TEST_CASE(empty_bid_ask_arrays) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "BTCUSDT",
        "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<DepthUpdate>(r));
    auto const& u = std::get<DepthUpdate>(r);
    BOOST_TEST(u.bids.empty());
    BOOST_TEST(u.asks.empty());
}

BOOST_AUTO_TEST_CASE(multiple_price_levels) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "BTCUSDT",
        "U": 1, "u": 5,
        "b": [["100.00000000","1.00000000"],["99.50000000","2.50000000"]],
        "a": [["101.00000000","0.50000000"],["101.50000000","3.00000000"],["102.00000000","1.00000000"]]
    })");
    BOOST_REQUIRE(std::holds_alternative<DepthUpdate>(r));
    auto const& u = std::get<DepthUpdate>(r);
    BOOST_TEST(u.bids.size() == 2u);
    BOOST_TEST(u.asks.size() == 3u);
    BOOST_TEST(u.bids[1].price == 9950000000LL);
    BOOST_TEST(u.bids[1].quantity == 250000000LL);
}

BOOST_AUTO_TEST_CASE(zero_quantity_deletion) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "BTCUSDT",
        "U": 1, "u": 1,
        "b": [["78450.06000000","0.00000000"]], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<DepthUpdate>(r));
    auto const& u = std::get<DepthUpdate>(r);
    BOOST_TEST(u.bids[0].quantity == 0LL);
}

BOOST_AUTO_TEST_CASE(invalid_json) {
    auto r = parse_depth_update("not json");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("invalid JSON") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(not_an_object) {
    auto r = parse_depth_update("[1, 2, 3]");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(missing_event_type) {
    auto r = parse_depth_update(R"({
        "E": 100, "s": "X", "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("\"e\"") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(wrong_event_type) {
    auto r = parse_depth_update(R"({
        "e": "trade", "E": 100, "s": "X", "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("unexpected event type") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_symbol) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(missing_timestamp) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "s": "X", "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(missing_first_update_id) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X", "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(invalid_sequence_range_U_greater_than_u) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 10, "u": 5, "b": [], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("sequence range") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(U_equals_u_is_valid) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 5, "u": 5, "b": [], "a": []
    })");
    BOOST_TEST(std::holds_alternative<DepthUpdate>(r));
}

BOOST_AUTO_TEST_CASE(price_level_not_array) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 1, "u": 1, "b": ["wrong"], "a": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(price_level_wrong_element_count) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 1, "u": 1, "b": [["100.0","1.0","extra"]], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("2 elements") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(price_level_numeric_not_string) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 1, "u": 1, "b": [[100.0, 1.0]], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("strings") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(malformed_price_string) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 1, "u": 1, "b": [["abc","1.0"]], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("price") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(malformed_quantity_string) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 1, "u": 1, "b": [["100.0","xyz"]], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("quantity") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(zero_price_rejected) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 1, "u": 1, "b": [["0.00000000","1.0"]], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("positive") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(negative_quantity_rejected) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 1, "u": 1, "b": [["100.0","-1.0"]], "a": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(excessive_decimal_precision_rejected) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 1, "u": 1, "b": [["100.123456789","1.0"]], "a": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(overflow_price_rejected) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 1, "u": 1, "b": [["99999999999999999.0","1.0"]], "a": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(atomic_failure_no_partial_result) {
    // Invalid ask array should not produce a DepthUpdate with valid bids.
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "BTCUSDT",
        "U": 1, "u": 1,
        "b": [["100.0","1.0"]],
        "a": [["bad","1.0"]]
    })");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(missing_bids_array) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X", "U": 1, "u": 2, "a": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(missing_asks_array) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X", "U": 1, "u": 2, "b": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(empty_symbol_rejected) {
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "",
        "U": 1, "u": 1, "b": [], "a": []
    })");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("empty symbol") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(fractional_overflow_through_parser) {
    // 92233720368.54775808 overflows INT64_MAX via the fractional part.
    // Must be caught as a parse error, not undefined behavior.
    auto r = parse_depth_update(R"({
        "e": "depthUpdate", "E": 100, "s": "X",
        "U": 1, "u": 1,
        "b": [["92233720368.54775808","1.0"]], "a": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_SUITE_END()

// --- bounded buffer test (M1.1) ---

BOOST_AUTO_TEST_SUITE(buffer)

BOOST_AUTO_TEST_CASE(flat_buffer_respects_max_size) {
    boost::beast::flat_buffer buf(64);
    BOOST_TEST(buf.max_size() == 64u);
}

BOOST_AUTO_TEST_SUITE_END()

// --- TLS test fixtures (shared by M1.1 and M2.2) ---

static constexpr char kTestCert[] = R"(-----BEGIN CERTIFICATE-----
MIIDNDCCAhygAwIBAgIUM5Nzg/6ZexgbDDGoVaabU8j6oHEwDQYJKoZIhvcNAQEL
BQAwGzEZMBcGA1UEAwwQdGVzdHNlcnZlci5sb2NhbDAeFw0yNjExMDMwNTU5NDda
Fw0zNjEwMzEwNTU5NDdaMBsxGTAXBgNVBAMMEHRlc3RzZXJ2ZXIubG9jYWwwggEi
MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCyB1rPZ11kAmXDe9dLR3qX7F+1
kLcMzswTwlFwIdxEVgftEN26OgtVZei6/R2tPPZZ7P+H962aC+r+E1r2aqtuP/hn
L8YwjljuAOQ/ldN+kPxbuIsFhJ8lq8VNXNxVd64pmxjkHU+81qxh1V0ygon0HbLP
0fvxGyqUeQNvBhG5g4R8NZyY42ML5rwrLD2Ibs7v8mZulORoc4ChNyQX2U/mvISw
0kHym6Szt4GxXyl3McJB7ZomjhsKZlwoZpRv/4AAcrZ9mCc3WSAQGIHPl1+HdUC/
wKL0rWoRiVCzNGYAZ4eqrFBE7z45RLzE23nuv9ZTV0B6c+I8FFCsdb30s5XZAgMB
AAGjcDBuMB0GA1UdDgQWBBTqR+enN7Pz8rQ42j/YL6m34l+QETAfBgNVHSMEGDAW
gBTqR+enN7Pz8rQ42j/YL6m34l+QETAPBgNVHRMBAf8EBTADAQH/MBsGA1UdEQQU
MBKCEHRlc3RzZXJ2ZXIubG9jYWwwDQYJKoZIhvcNAQELBQADggEBAFSghbhLZ9DI
doo2QFBuJgbG3F11Z8Vi+mkC3YK+QVDXFMCN4VtF2Kzq2Hw0EpTD28RXt8Zf8AwD
cNF766pUs2YLQ/8Vts+LZABLI2greU9OwMI6b3KfxYYdSH1rGntMq7nRuoIlwz1d
FnKF30kb7SFVCO2jF4TXlJ0kv+/g4ug+jVT7VBRAvhfjmBUfLgEyBw1fAZ979h36
JOBXZ3rsYrmzF/uDLaIWLeoir1BU5ZMBIacVMQ90fftDGJu7Ftdidp2kozwWbOR9
L224ltN70rW/4iDnqJnkZpp1UZFgq9FCBIazpQvsbA0Er1wmXsbVUCnTWDx3b/x6
XgAcuI2ggVI=
-----END CERTIFICATE-----)";

static constexpr char kTestKey[] = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCyB1rPZ11kAmXD
e9dLR3qX7F+1kLcMzswTwlFwIdxEVgftEN26OgtVZei6/R2tPPZZ7P+H962aC+r+
E1r2aqtuP/hnL8YwjljuAOQ/ldN+kPxbuIsFhJ8lq8VNXNxVd64pmxjkHU+81qxh
1V0ygon0HbLP0fvxGyqUeQNvBhG5g4R8NZyY42ML5rwrLD2Ibs7v8mZulORoc4Ch
NyQX2U/mvISw0kHym6Szt4GxXyl3McJB7ZomjhsKZlwoZpRv/4AAcrZ9mCc3WSAQ
GIHPl1+HdUC/wKL0rWoRiVCzNGYAZ4eqrFBE7z45RLzE23nuv9ZTV0B6c+I8FFCs
db30s5XZAgMBAAECggEABMifzwzm/IX3+A2+1j4AdE6cr8NIt+Zio3FIQOwYABoT
snIOky92GIBZhZTBlGMb94SfxSaJmqcRVGxuuEpbeuXrJdKpz6ozh4msmGzNNwxs
S4ak7awHnrqD3bVOElnZ6zJIs/B5sQhPr31NOMs7K70UjfMFgxAIz96EkhompcZw
cta6Dl4u6eX4ReR834lIVfcineLPYKSIZAYz2lk7s1CxBXx6V+3wN+68du6/dtPq
q/L74+t3LF+KC5ngcvVeLocOif0SwoNvL97giyxce4yspIJwnCh16uGlLRfoDtWP
oweATF3J547KWA3F3t3IU1I1/go65FrUhWXvIYpPZQKBgQDV2oPWUt9xbRW93HVS
oy9cN6W5Pc470gatvbTjY8hNeSiQ/y6uZu5JA74bmk+sqyB+HyDCXDTQMDP18lyc
t1hRK8+6U/knn+RhC5e7biBESZoRf3/TqsMZqWab5mE2qwKRcU3pmdgnl2I4gdTb
700HDhLLJoX4ghHY32akV+9+wwKBgQDVHWFmKztvWvHSkg53LQn4R9vIhHK9mcgc
zVN1+YqtvelhtvH04rbJK9ZzqGbMYJULwEm9ozEn/IvjNeenEFbFmLVwBHZUsb15
xrXru+aC56C3lUikdmOLFlrPu15Iqn4K7Sx9qAZUzRrKNVgJVbRYMiVNmtap0sNl
A8xMIgIHMwKBgC+0BbKaW+N2XOI0OiuL4TWl6B7RLZ7kc77v0Vjj2juF6kml0qju
pvq8jfykSF+Q8LeRhAI3BUTna7/SJ1yB/X5l0nmS38I6BXysVFVn9GSnMmny0fXK
Yqjnw87E7ix+qxz8dE/GlJQITc2jEHNbBwiq2ABfAIV1pjuFYeUJBJB3AoGAIA51
Uir6m7twWJ5DssWBlGkqtDTIFaEGKt7vyPUqWQwSCPK93nojyIrhjTAtllyIzuWz
seTURhGE8T5wg/6/LqXT1O+25PJ/b8vmHXl8KQCyeirobGSqgIdyTYD/o6qRnw8h
/BzEJwlbGEOB5JWRtRqF5fB+jbj4Tz+NAX60E2UCgYEAxx3Zz3KVodhrARByydXM
tYbCk+kpZ0FQblj+Ctj5shiAtEwxZxbj7UoXduPJFp+qJZWAaHGu0P8aEcNLNobU
xEpps7lFh3NB14EIreRcOkoYQkP8Wx0JvsaF7ckii/dGvL9nLCpxBLQqRIGgWl6p
01RlX7VXAT2ogYGoET1dlyc=
-----END PRIVATE KEY-----)";

// --- TLS hostname verification (M1.1) ---

BOOST_AUTO_TEST_SUITE(tls)

BOOST_AUTO_TEST_CASE(ssl_set1_host_configures_verification) {
    ssl::context ctx(ssl::context::tlsv12_client);
    SSL* handle = SSL_new(ctx.native_handle());
    BOOST_REQUIRE(handle != nullptr);

    BOOST_TEST(SSL_set_tlsext_host_name(handle, "example.com") == 1);
    BOOST_TEST(SSL_set1_host(handle, "example.com") == 1);

    X509_VERIFY_PARAM* param = SSL_get0_param(handle);
    BOOST_REQUIRE(param != nullptr);

    SSL_free(handle);
}

BOOST_AUTO_TEST_CASE(hostname_mismatch_rejected_by_local_server) {
    net::io_context ioc;

    ssl::context server_ctx(ssl::context::tlsv12_server);
    server_ctx.use_certificate_chain(
        net::buffer(kTestCert, std::strlen(kTestCert)));
    server_ctx.use_private_key(
        net::buffer(kTestKey, std::strlen(kTestKey)), ssl::context::pem);

    beast::error_code bind_ec;
    tcp::acceptor acceptor(ioc);
    acceptor.open(tcp::v4(), bind_ec);
    if (bind_ec) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot open socket");
        return;
    }
    acceptor.bind(tcp::endpoint(net::ip::make_address("127.0.0.1"), 0), bind_ec);
    if (bind_ec) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind (sandbox?)");
        return;
    }
    acceptor.listen(1, bind_ec);
    if (bind_ec) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot listen");
        return;
    }
    auto port = std::to_string(acceptor.local_endpoint().port());

    ssl::stream<tcp::socket> server_stream(ioc, server_ctx);
    acceptor.async_accept(server_stream.next_layer(),
        [&server_stream](beast::error_code ec) {
            if (!ec) {
                server_stream.async_handshake(ssl::stream_base::server,
                    [](beast::error_code) {});
            }
        });

    ssl::context client_ctx(ssl::context::tlsv12_client);
    client_ctx.add_certificate_authority(
        net::buffer(kTestCert, std::strlen(kTestCert)));
    client_ctx.set_verify_mode(ssl::verify_peer);

    std::atomic<bool> error_fired{false};
    std::string error_stage;
    auto session = std::make_shared<Session>(
        ioc, client_ctx, "localhost", port, "/",
        [&error_fired, &error_stage](beast::error_code, char const* what) {
            error_fired = true;
            error_stage = what;
        });
    session->run();

    ioc.run();

    BOOST_TEST(error_fired.load());
    BOOST_TEST(error_stage == "ssl_handshake");
}

BOOST_AUTO_TEST_SUITE_END()

// --- Session lifecycle tests (M1.1) ---

BOOST_AUTO_TEST_SUITE(session_lifecycle)

BOOST_AUTO_TEST_CASE(stop_during_resolve_does_not_advance) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);

    std::atomic<bool> error_called{false};
    auto session = std::make_shared<Session>(
        ioc, ctx, "nonexistent.test.invalid", "9443", "/ws/test",
        [&error_called](beast::error_code, char const*) {
            error_called = true;
        });

    session->run();
    session->stop();
    ioc.run();

    auto const& stats = session->stats();
    BOOST_TEST(stats.messages_received == 0u);
    BOOST_TEST(
        (stats.connected_at == std::chrono::steady_clock::time_point{}));
}

BOOST_AUTO_TEST_CASE(stop_before_run_is_safe) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);

    auto session = std::make_shared<Session>(
        ioc, ctx, "example.com", "443", "/");

    session->stop();
    ioc.run();

    auto const& stats = session->stats();
    BOOST_TEST(stats.messages_received == 0u);
    BOOST_TEST(stats.bytes_received == 0u);
}

BOOST_AUTO_TEST_CASE(error_callback_fires_on_dns_failure) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);

    std::atomic<bool> error_called{false};
    std::string error_stage;
    auto session = std::make_shared<Session>(
        ioc, ctx, "nonexistent.test.invalid", "9443", "/ws/test",
        [&error_called, &error_stage](beast::error_code, char const* what) {
            error_called = true;
            error_stage = what;
        });

    session->run();
    ioc.run();

    BOOST_TEST(error_called.load());
    BOOST_TEST(error_stage == "resolve");
}

BOOST_AUTO_TEST_CASE(stats_zero_when_never_connected) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);

    auto session = std::make_shared<Session>(
        ioc, ctx, "nonexistent.test.invalid", "9443", "/ws/test");

    session->run();
    session->stop();
    ioc.run();

    auto const& stats = session->stats();
    BOOST_TEST(stats.messages_received == 0u);
    BOOST_TEST(stats.bytes_received == 0u);
    BOOST_TEST(stats.invalid_messages == 0u);
    BOOST_TEST(
        (stats.connected_at == std::chrono::steady_clock::time_point{}));
}

BOOST_AUTO_TEST_SUITE_END()

// --- format_scaled tests (M2.1) ---

BOOST_AUTO_TEST_SUITE(format_scaled_tests)

BOOST_AUTO_TEST_CASE(whole_number) {
    BOOST_TEST(format_scaled(100'000'000LL) == "1");
}

BOOST_AUTO_TEST_CASE(zero_value) {
    BOOST_TEST(format_scaled(0) == "0");
}

BOOST_AUTO_TEST_CASE(fractional_trimmed) {
    // 78450.06 = 7845006000000
    BOOST_TEST(format_scaled(7845006000000LL) == "78450.06");
}

BOOST_AUTO_TEST_CASE(full_precision_no_trim) {
    // 0.12345678 = 12345678
    BOOST_TEST(format_scaled(12345678LL) == "0.12345678");
}

BOOST_AUTO_TEST_CASE(trailing_zeros_trimmed) {
    // 1.5 = 150000000
    BOOST_TEST(format_scaled(150'000'000LL) == "1.5");
}

BOOST_AUTO_TEST_CASE(large_value) {
    // 92233720368.54775807 = INT64_MAX
    BOOST_TEST(format_scaled(std::numeric_limits<int64_t>::max()) ==
               "92233720368.54775807");
}

BOOST_AUTO_TEST_CASE(single_satoshi) {
    // 0.00000001 = 1
    BOOST_TEST(format_scaled(1LL) == "0.00000001");
}

BOOST_AUTO_TEST_CASE(negative_value) {
    BOOST_TEST(format_scaled(-150'000'000LL) == "-1.5");
}

BOOST_AUTO_TEST_CASE(int64_min) {
    // INT64_MIN = -9223372036854775808 → -92233720368.54775808
    auto result = format_scaled(std::numeric_limits<int64_t>::min());
    BOOST_TEST(result == "-92233720368.54775808");
}

BOOST_AUTO_TEST_SUITE_END()

// --- snapshot parsing tests (M2.1) ---

BOOST_AUTO_TEST_SUITE(snapshot_parsing)

static const std::string kValidSnapshot = R"({
    "lastUpdateId": 1027024,
    "bids": [["4.00000000","431.00000000"],["3.99000000","5.00000000"]],
    "asks": [["4.00000200","12.00000000"]]
})";

BOOST_AUTO_TEST_CASE(valid_snapshot_parses) {
    auto r = parse_snapshot(kValidSnapshot);
    BOOST_REQUIRE(std::holds_alternative<Snapshot>(r));
    auto const& s = std::get<Snapshot>(r);
    BOOST_TEST(s.last_update_id == 1027024ULL);
    BOOST_TEST(s.bids.size() == 2u);
    BOOST_TEST(s.asks.size() == 1u);
    BOOST_TEST(s.bids[0].price == 400000000LL);
    BOOST_TEST(s.bids[0].quantity == 43100000000LL);
    BOOST_TEST(s.bids[1].price == 399000000LL);
    BOOST_TEST(s.asks[0].price == 400000200LL);
}

BOOST_AUTO_TEST_CASE(empty_bids_and_asks) {
    auto r = parse_snapshot(R"({"lastUpdateId": 100, "bids": [], "asks": []})");
    BOOST_REQUIRE(std::holds_alternative<Snapshot>(r));
    auto const& s = std::get<Snapshot>(r);
    BOOST_TEST(s.last_update_id == 100ULL);
    BOOST_TEST(s.bids.empty());
    BOOST_TEST(s.asks.empty());
}

BOOST_AUTO_TEST_CASE(zero_quantity_entries_skipped) {
    auto r = parse_snapshot(R"({
        "lastUpdateId": 100,
        "bids": [["1.0","0.00000000"],["2.0","1.0"]],
        "asks": [["3.0","0.00000000"]]
    })");
    BOOST_REQUIRE(std::holds_alternative<Snapshot>(r));
    auto const& s = std::get<Snapshot>(r);
    BOOST_TEST(s.bids.size() == 1u);
    BOOST_TEST(s.bids[0].price == 200000000LL);
    BOOST_TEST(s.asks.empty());
}

BOOST_AUTO_TEST_CASE(missing_last_update_id) {
    auto r = parse_snapshot(R"({"bids": [], "asks": []})");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(missing_bids) {
    auto r = parse_snapshot(R"({"lastUpdateId": 100, "asks": []})");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(missing_asks) {
    auto r = parse_snapshot(R"({"lastUpdateId": 100, "bids": []})");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(invalid_json) {
    auto r = parse_snapshot("not json");
    BOOST_REQUIRE(std::holds_alternative<ParseError>(r));
    BOOST_TEST(std::get<ParseError>(r).message.find("invalid JSON") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(not_an_object) {
    auto r = parse_snapshot("[1,2,3]");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(negative_last_update_id) {
    auto r = parse_snapshot(R"({"lastUpdateId": -1, "bids": [], "asks": []})");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_CASE(malformed_price_in_snapshot) {
    auto r = parse_snapshot(R"({
        "lastUpdateId": 100,
        "bids": [["abc","1.0"]], "asks": []
    })");
    BOOST_TEST(std::holds_alternative<ParseError>(r));
}

BOOST_AUTO_TEST_SUITE_END()

// --- order book tests (M2.1) ---

BOOST_AUTO_TEST_SUITE(order_book)

// Helper to build a DepthUpdate for testing.
static DepthUpdate make_update(uint64_t U, uint64_t u,
                               std::vector<PriceLevel> bids = {},
                               std::vector<PriceLevel> asks = {}) {
    return DepthUpdate{"BTCUSDT", 0, U, u, std::move(bids), std::move(asks)};
}

BOOST_AUTO_TEST_CASE(empty_book_has_no_best) {
    OrderBook book;
    BOOST_TEST(!book.best_bid().has_value());
    BOOST_TEST(!book.best_ask().has_value());
    BOOST_TEST(book.update_id() == 0u);
    BOOST_TEST(book.bid_count() == 0u);
    BOOST_TEST(book.ask_count() == 0u);
}

BOOST_AUTO_TEST_CASE(apply_snapshot_replaces_book) {
    OrderBook book;

    Snapshot snap{1000,
                  {{200'000'000, 500'000'000}, {100'000'000, 300'000'000}},
                  {{300'000'000, 100'000'000}}};
    book.apply_snapshot(snap);

    BOOST_TEST(book.update_id() == 1000u);
    BOOST_TEST(book.bid_count() == 2u);
    BOOST_TEST(book.ask_count() == 1u);

    auto bb = book.best_bid();
    BOOST_REQUIRE(bb.has_value());
    BOOST_TEST(bb->first == 200'000'000);
    BOOST_TEST(bb->second == 500'000'000);

    auto ba = book.best_ask();
    BOOST_REQUIRE(ba.has_value());
    BOOST_TEST(ba->first == 300'000'000);
    BOOST_TEST(ba->second == 100'000'000);
}

BOOST_AUTO_TEST_CASE(snapshot_clears_previous_state) {
    OrderBook book;

    Snapshot snap1{100, {{500, 100}}, {{600, 200}}};
    book.apply_snapshot(snap1);
    BOOST_TEST(book.bid_count() == 1u);

    Snapshot snap2{200, {}, {{700, 300}}};
    book.apply_snapshot(snap2);
    BOOST_TEST(book.bid_count() == 0u);
    BOOST_TEST(book.ask_count() == 1u);
    BOOST_TEST(book.update_id() == 200u);
    BOOST_TEST(!book.best_bid().has_value());
}

BOOST_AUTO_TEST_CASE(apply_update_inserts_new_level) {
    OrderBook book;
    Snapshot snap{100, {}, {}};
    book.apply_snapshot(snap);

    auto result = book.apply_update(
        make_update(101, 101, {{500, 100}}, {}));
    BOOST_TEST((result == UpdateResult::Applied));
    BOOST_TEST(book.bid_count() == 1u);
    BOOST_TEST(book.best_bid()->first == 500);
}

BOOST_AUTO_TEST_CASE(apply_update_replaces_quantity) {
    OrderBook book;
    Snapshot snap{100, {{500, 100}}, {}};
    book.apply_snapshot(snap);

    auto result = book.apply_update(
        make_update(101, 101, {{500, 999}}, {}));
    BOOST_TEST((result == UpdateResult::Applied));
    BOOST_TEST(book.best_bid()->second == 999);
}

BOOST_AUTO_TEST_CASE(apply_update_deletes_zero_quantity) {
    OrderBook book;
    Snapshot snap{100, {{500, 100}, {400, 200}}, {}};
    book.apply_snapshot(snap);

    auto result = book.apply_update(
        make_update(101, 101, {{500, 0}}, {}));
    BOOST_TEST((result == UpdateResult::Applied));
    BOOST_TEST(book.bid_count() == 1u);
    BOOST_TEST(book.best_bid()->first == 400);
}

BOOST_AUTO_TEST_CASE(delete_nonexistent_is_noop) {
    OrderBook book;
    Snapshot snap{100, {{500, 100}}, {}};
    book.apply_snapshot(snap);

    auto result = book.apply_update(
        make_update(101, 101, {{999, 0}}, {}));
    BOOST_TEST((result == UpdateResult::Applied));
    BOOST_TEST(book.bid_count() == 1u);
}

BOOST_AUTO_TEST_CASE(stale_update_ignored) {
    OrderBook book;
    Snapshot snap{100, {{500, 100}}, {}};
    book.apply_snapshot(snap);

    auto result = book.apply_update(make_update(50, 99, {{500, 999}}, {}));
    BOOST_TEST((result == UpdateResult::Stale));
    BOOST_TEST(book.best_bid()->second == 100);
    BOOST_TEST(book.update_id() == 100u);
}

BOOST_AUTO_TEST_CASE(stale_update_u_equals_update_id) {
    OrderBook book;
    Snapshot snap{100, {}, {}};
    book.apply_snapshot(snap);

    auto result = book.apply_update(make_update(99, 100, {{500, 100}}, {}));
    BOOST_TEST((result == UpdateResult::Stale));
}

BOOST_AUTO_TEST_CASE(gap_detected) {
    OrderBook book;
    Snapshot snap{100, {{500, 100}}, {}};
    book.apply_snapshot(snap);

    // U=103 but book is at 100, so expected U <= 101.
    auto result = book.apply_update(make_update(103, 105, {{500, 999}}, {}));
    BOOST_TEST((result == UpdateResult::Gap));
    BOOST_TEST(book.best_bid()->second == 100);
    BOOST_TEST(book.update_id() == 100u);
}

BOOST_AUTO_TEST_CASE(sequential_updates_advance_id) {
    OrderBook book;
    Snapshot snap{100, {}, {}};
    book.apply_snapshot(snap);

    BOOST_TEST((book.apply_update(
        make_update(101, 105, {{500, 100}}, {})) == UpdateResult::Applied));
    BOOST_TEST(book.update_id() == 105u);

    BOOST_TEST((book.apply_update(
        make_update(106, 110, {{600, 200}}, {})) == UpdateResult::Applied));
    BOOST_TEST(book.update_id() == 110u);
    BOOST_TEST(book.bid_count() == 2u);
}

BOOST_AUTO_TEST_CASE(update_empties_book) {
    OrderBook book;
    Snapshot snap{100, {{500, 100}}, {{600, 200}}};
    book.apply_snapshot(snap);

    auto result = book.apply_update(
        make_update(101, 101, {{500, 0}}, {{600, 0}}));
    BOOST_TEST((result == UpdateResult::Applied));
    BOOST_TEST(book.bid_count() == 0u);
    BOOST_TEST(book.ask_count() == 0u);
    BOOST_TEST(!book.best_bid().has_value());
    BOOST_TEST(!book.best_ask().has_value());
}

BOOST_AUTO_TEST_CASE(best_bid_is_highest_price) {
    OrderBook book;
    Snapshot snap{100, {{100, 10}, {300, 30}, {200, 20}}, {}};
    book.apply_snapshot(snap);

    BOOST_TEST(book.best_bid()->first == 300);
    BOOST_TEST(book.best_bid()->second == 30);
}

BOOST_AUTO_TEST_CASE(best_ask_is_lowest_price) {
    OrderBook book;
    Snapshot snap{100, {}, {{300, 30}, {100, 10}, {200, 20}}};
    book.apply_snapshot(snap);

    BOOST_TEST(book.best_ask()->first == 100);
    BOOST_TEST(book.best_ask()->second == 10);
}

BOOST_AUTO_TEST_CASE(bridging_update_U_leq_update_id_plus_1) {
    // The first update after a snapshot may have U <= lastUpdateId + 1
    // as long as u > lastUpdateId (the bridging event from step 5).
    OrderBook book;
    Snapshot snap{100, {}, {}};
    book.apply_snapshot(snap);

    // U=98, u=103 — U <= 101 and u > 100 → should apply.
    auto result = book.apply_update(
        make_update(98, 103, {{500, 100}}, {}));
    BOOST_TEST((result == UpdateResult::Applied));
    BOOST_TEST(book.update_id() == 103u);
}

BOOST_AUTO_TEST_CASE(snapshot_with_empty_one_side) {
    OrderBook book;
    Snapshot snap{100, {{500, 100}}, {}};
    book.apply_snapshot(snap);

    BOOST_TEST(book.bid_count() == 1u);
    BOOST_TEST(book.ask_count() == 0u);
    BOOST_TEST(book.best_bid().has_value());
    BOOST_TEST(!book.best_ask().has_value());
}

BOOST_AUTO_TEST_CASE(update_on_uninitialized_book_is_gap) {
    // Book starts with update_id=0. An update with U=1 would need
    // update_id+1=1, which matches U=1 → Applied.
    // But an update with U=5 → Gap.
    OrderBook book;
    auto result = book.apply_update(make_update(5, 10, {{500, 100}}, {}));
    BOOST_TEST((result == UpdateResult::Gap));
}

BOOST_AUTO_TEST_CASE(snapshot_zero_quantity_not_stored) {
    // Zero-quantity levels in a directly constructed Snapshot must not
    // appear in the book, even when bypassing parse_snapshot().
    OrderBook book;
    Snapshot snap{100, {{500, 0}, {400, 100}}, {{600, 0}, {700, 200}}};
    book.apply_snapshot(snap);

    BOOST_TEST(book.bid_count() == 1u);
    BOOST_TEST(book.ask_count() == 1u);
    BOOST_TEST(book.best_bid()->first == 400);
    BOOST_TEST(book.best_ask()->first == 700);
}

BOOST_AUTO_TEST_CASE(uint64_max_update_id_all_updates_stale) {
    // When update_id_ == UINT64_MAX, every possible u fits in uint64 and
    // satisfies u <= UINT64_MAX, so the stale check rejects all updates
    // before the overflow guard is reached. The guard (update_id_ ==
    // UINT64_MAX → Gap) is retained defensively in case the check order
    // changes, but is currently unreachable from this state.
    OrderBook book;
    Snapshot snap{UINT64_MAX, {{500, 100}}, {}};
    book.apply_snapshot(snap);
    BOOST_TEST(book.update_id() == UINT64_MAX);

    // u=1 < UINT64_MAX → Stale.
    auto result = book.apply_update(make_update(0, 1, {{500, 200}}, {}));
    BOOST_TEST((result == UpdateResult::Stale));

    // u=UINT64_MAX == update_id_ → Stale (<=, not <).
    auto result2 = book.apply_update(
        make_update(0, UINT64_MAX, {{500, 300}}, {}));
    BOOST_TEST((result2 == UpdateResult::Stale));

    // Verify the book was never modified.
    BOOST_TEST(book.best_bid()->second == 100);

    // Verify the last valid transition into UINT64_MAX works correctly.
    Snapshot snap2{UINT64_MAX - 1, {{500, 100}}, {}};
    book.apply_snapshot(snap2);

    auto result3 = book.apply_update(
        make_update(UINT64_MAX, UINT64_MAX, {{500, 200}}, {}));
    BOOST_TEST((result3 == UpdateResult::Applied));
    BOOST_TEST(book.update_id() == UINT64_MAX);
    BOOST_TEST(book.best_bid()->second == 200);
}

BOOST_AUTO_TEST_CASE(locked_book_no_warning) {
    // best_bid == best_ask (locked book) must not trigger a crossed-book
    // warning. Only best_bid > best_ask is crossed.
    OrderBook book;
    Snapshot snap{100, {{500, 100}}, {{500, 200}}};
    book.apply_snapshot(snap);

    // Capture stderr to verify no warning is emitted.
    std::ostringstream captured;
    auto* original = std::cerr.rdbuf(captured.rdbuf());

    auto result = book.apply_update(
        make_update(101, 101, {{500, 150}}, {}));

    // Restore stderr before any assertion can fail and skip cleanup.
    std::cerr.rdbuf(original);

    BOOST_TEST((result == UpdateResult::Applied));
    BOOST_TEST(book.best_bid()->first == 500);
    BOOST_TEST(book.best_ask()->first == 500);
    BOOST_TEST(captured.str().find("crossed") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// --- Shared mock HTTPS server for HttpClient and SyncEngine tests ---

#include "http_client.hpp"

struct MockServer {
    net::io_context& ioc;
    ssl::context server_ctx{ssl::context::tlsv12_server};
    tcp::acceptor acceptor;
    std::string port;
    bool available = false;

    explicit MockServer(net::io_context& ioc_) : ioc(ioc_), acceptor(ioc_) {
        server_ctx.use_certificate_chain(
            net::buffer(kTestCert, std::strlen(kTestCert)));
        server_ctx.use_private_key(
            net::buffer(kTestKey, std::strlen(kTestKey)), ssl::context::pem);

        beast::error_code ec;
        acceptor.open(tcp::v4(), ec);
        if (ec) return;
        acceptor.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) return;
        acceptor.bind(tcp::endpoint(net::ip::make_address("127.0.0.1"), 0), ec);
        if (ec) return;
        acceptor.listen(4, ec);
        if (ec) return;
        port = std::to_string(acceptor.local_endpoint().port());
        available = true;
    }

    // Accept one connection and serve a canned response after reading
    // the HTTP request. keep_alive controls the Connection header.
    void serve(std::string body, unsigned status = 200,
               bool keep_alive = true) {
        auto server_stream =
            std::make_shared<ssl::stream<tcp::socket>>(ioc, server_ctx);
        acceptor.async_accept(server_stream->next_layer(),
            [server_stream, body = std::move(body), status, keep_alive]
            (beast::error_code ec) mutable {
                if (ec) return;
                server_stream->async_handshake(ssl::stream_base::server,
                    [server_stream, body = std::move(body), status, keep_alive]
                    (beast::error_code ec) mutable {
                        if (ec) return;
                        auto req = std::make_shared<
                            http::request<http::string_body>>();
                        auto buf = std::make_shared<beast::flat_buffer>();
                        http::async_read(*server_stream, *buf, *req,
                            [server_stream, req, buf,
                             body = std::move(body), status, keep_alive]
                            (beast::error_code ec, std::size_t) mutable {
                                if (ec) return;
                                auto res = std::make_shared<
                                    http::response<http::string_body>>();
                                res->result(status);
                                res->version(11);
                                res->set(http::field::content_type,
                                         "application/json");
                                res->keep_alive(keep_alive);
                                res->body() = std::move(body);
                                res->prepare_payload();
                                http::async_write(*server_stream, *res,
                                    [server_stream, res, keep_alive]
                                    (beast::error_code, std::size_t) {
                                        if (!keep_alive) {
                                            beast::error_code ec;
                                            server_stream->next_layer()
                                                .close(ec);
                                        }
                                    });
                            });
                    });
            });
    }

    ssl::context make_client_ctx() {
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.add_certificate_authority(
            net::buffer(kTestCert, std::strlen(kTestCert)));
        ctx.set_verify_mode(ssl::verify_peer);
        return ctx;
    }
};

// --- HttpClient tests (M2.2) ---

BOOST_AUTO_TEST_SUITE(http_client)

BOOST_AUTO_TEST_CASE(successful_get_returns_body) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    std::string response_body = R"({"lastUpdateId":100,"bids":[],"asks":[]})";
    srv.serve(response_body);

    HttpClient::Result got;
    auto client = std::make_shared<HttpClient>(
        ioc, ctx, "testserver.local", srv.port);
    client->async_get("/api/v3/depth", [&got](auto r) { got = std::move(r); });

    ioc.run();

    auto* resp = std::get_if<HttpClient::Response>(&got);
    BOOST_REQUIRE(resp != nullptr);
    BOOST_TEST(resp->status == 200u);
    BOOST_TEST(resp->body == response_body);
}

BOOST_AUTO_TEST_CASE(non_200_status_returned) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    srv.serve(R"({"code":-1003,"msg":"Too many requests."})", 429);

    HttpClient::Result got;
    auto client = std::make_shared<HttpClient>(
        ioc, ctx, "testserver.local", srv.port);
    client->async_get("/api/v3/depth", [&got](auto r) { got = std::move(r); });

    ioc.run();

    auto* resp = std::get_if<HttpClient::Response>(&got);
    BOOST_REQUIRE(resp != nullptr);
    BOOST_TEST(resp->status == 429u);
}

BOOST_AUTO_TEST_CASE(connection_reuse_second_request) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // Server accepts one connection, serves two requests.
    auto server_stream =
        std::make_shared<ssl::stream<tcp::socket>>(ioc, srv.server_ctx);
    srv.acceptor.async_accept(server_stream->next_layer(),
        [server_stream](beast::error_code ec) {
            if (ec) return;
            server_stream->async_handshake(ssl::stream_base::server,
                [server_stream](beast::error_code ec) {
                    if (ec) return;
                    // Read+respond to first request.
                    auto req1 = std::make_shared<
                        http::request<http::string_body>>();
                    auto buf = std::make_shared<beast::flat_buffer>();
                    http::async_read(*server_stream, *buf, *req1,
                        [server_stream, req1, buf](beast::error_code ec,
                                                    std::size_t) {
                            if (ec) return;
                            auto res1 = std::make_shared<
                                http::response<http::string_body>>();
                            res1->result(200);
                            res1->version(11);
                            res1->keep_alive(true);
                            res1->body() = "first";
                            res1->prepare_payload();
                            http::async_write(*server_stream, *res1,
                                [server_stream, res1, buf]
                                (beast::error_code ec, std::size_t) {
                                    if (ec) return;
                                    buf->consume(buf->size());
                                    // Read+respond to second request.
                                    auto req2 = std::make_shared<
                                        http::request<http::string_body>>();
                                    http::async_read(*server_stream, *buf,
                                        *req2,
                                        [server_stream, req2, buf]
                                        (beast::error_code ec, std::size_t) {
                                            if (ec) return;
                                            auto res2 = std::make_shared<
                                                http::response<
                                                    http::string_body>>();
                                            res2->result(200);
                                            res2->version(11);
                                            res2->keep_alive(true);
                                            res2->body() = "second";
                                            res2->prepare_payload();
                                            http::async_write(*server_stream,
                                                *res2,
                                                [server_stream, res2]
                                                (beast::error_code, std::size_t)
                                                {});
                                        });
                                });
                        });
                });
        });

    std::string body1, body2;
    auto client = std::make_shared<HttpClient>(
        ioc, ctx, "testserver.local", srv.port);

    client->async_get("/first", [&](auto r) {
        auto* resp = std::get_if<HttpClient::Response>(&r);
        if (resp) body1 = resp->body;
        // Issue second request from the first's callback.
        client->async_get("/second", [&](auto r2) {
            auto* resp2 = std::get_if<HttpClient::Response>(&r2);
            if (resp2) body2 = resp2->body;
        });
    });

    ioc.run();

    BOOST_TEST(body1 == "first");
    BOOST_TEST(body2 == "second");
}

BOOST_AUTO_TEST_CASE(connection_failure_reports_error) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    ctx.set_verify_mode(ssl::verify_peer);

    HttpClient::Result got;
    // Port 1 — nothing is listening there.
    auto client = std::make_shared<HttpClient>(
        ioc, ctx, "127.0.0.1", "1");
    client->async_get("/test", [&got](auto r) { got = std::move(r); });

    ioc.run();

    auto* err = std::get_if<std::string>(&got);
    BOOST_REQUIRE(err != nullptr);
    BOOST_TEST(err->find("connect:") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(cancel_delivers_cancelled_result) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // Server accepts but never responds — client will hang until cancelled.
    auto server_stream =
        std::make_shared<ssl::stream<tcp::socket>>(ioc, srv.server_ctx);
    srv.acceptor.async_accept(server_stream->next_layer(),
        [server_stream](beast::error_code ec) {
            if (ec) return;
            server_stream->async_handshake(ssl::stream_base::server,
                [](beast::error_code) {});
        });

    HttpClient::Result got;
    bool callback_fired = false;
    auto client = std::make_shared<HttpClient>(
        ioc, ctx, "testserver.local", srv.port);
    client->async_get("/hang", [&](auto r) {
        callback_fired = true;
        got = std::move(r);
    });

    // Post cancel — runs after pending handlers get a chance to start.
    net::post(ioc, [&client]() { client->cancel(); });

    ioc.run();

    BOOST_TEST(callback_fired);
    auto* err = std::get_if<std::string>(&got);
    BOOST_REQUIRE(err != nullptr);
}

BOOST_AUTO_TEST_CASE(timeout_fires_on_unresponsive_server) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // Server accepts TLS but never sends a response.
    auto server_stream =
        std::make_shared<ssl::stream<tcp::socket>>(ioc, srv.server_ctx);
    srv.acceptor.async_accept(server_stream->next_layer(),
        [server_stream](beast::error_code ec) {
            if (ec) return;
            server_stream->async_handshake(ssl::stream_base::server,
                [](beast::error_code) {});
        });

    HttpClient::Result got;
    // 100ms timeout for test speed.
    auto client = std::make_shared<HttpClient>(
        ioc, ctx, "testserver.local", srv.port,
        std::chrono::milliseconds(100));
    client->async_get("/slow", [&got](auto r) { got = std::move(r); });

    ioc.run();

    auto* err = std::get_if<std::string>(&got);
    BOOST_REQUIRE(err != nullptr);
    BOOST_TEST(*err == "timeout");
}

BOOST_AUTO_TEST_CASE(tls_hostname_mismatch_rejected) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // Server accepts connections normally.
    srv.serve("should not arrive");

    HttpClient::Result got;
    // Use "localhost" which does not match cert SAN "testserver.local".
    auto client = std::make_shared<HttpClient>(
        ioc, ctx, "localhost", srv.port);
    client->async_get("/test", [&got](auto r) { got = std::move(r); });

    ioc.run();

    auto* err = std::get_if<std::string>(&got);
    BOOST_REQUIRE(err != nullptr);
    BOOST_TEST(err->find("tls_handshake:") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(stale_connection_recovery) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // First request: server responds then closes the connection.
    srv.serve("first", 200, false); // keep_alive = false

    std::string body1;
    HttpClient::Result got2;
    auto client = std::make_shared<HttpClient>(
        ioc, ctx, "testserver.local", srv.port);

    client->async_get("/first", [&](auto r) {
        auto* resp = std::get_if<HttpClient::Response>(&r);
        if (resp) body1 = resp->body;

        // Second request: server serves on a new connection.
        srv.serve("second", 200, true);
        client->async_get("/second", [&got2](auto r2) {
            got2 = std::move(r2);
        });
    });

    ioc.run();

    BOOST_TEST(body1 == "first");
    auto* resp2 = std::get_if<HttpClient::Response>(&got2);
    BOOST_REQUIRE(resp2 != nullptr);
    BOOST_TEST(resp2->body == "second");
}

BOOST_AUTO_TEST_CASE(overlapping_request_rejected) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // Server accepts but never responds — first request stays in flight.
    auto server_stream =
        std::make_shared<ssl::stream<tcp::socket>>(ioc, srv.server_ctx);
    srv.acceptor.async_accept(server_stream->next_layer(),
        [server_stream](beast::error_code ec) {
            if (ec) return;
            server_stream->async_handshake(ssl::stream_base::server,
                [](beast::error_code) {});
        });

    auto client = std::make_shared<HttpClient>(
        ioc, ctx, "testserver.local", srv.port,
        std::chrono::milliseconds(200));

    HttpClient::Result overlap_result;
    client->async_get("/first", [](auto) {});

    // Immediately try a second request while first is in flight.
    client->async_get("/second", [&overlap_result](auto r) {
        overlap_result = std::move(r);
    });

    // The second callback fires synchronously with a busy error.
    auto* err = std::get_if<std::string>(&overlap_result);
    BOOST_REQUIRE(err != nullptr);
    BOOST_TEST(*err == "request already in flight");

    // Let the first request time out so ioc.run() completes.
    client->cancel();
    ioc.run();
}

BOOST_AUTO_TEST_SUITE_END()

// --- SyncEngine tests (M2 Phase 1) ---

#include "sync_engine.hpp"

BOOST_AUTO_TEST_SUITE(sync_engine)

// Helper: make a DepthUpdate for testing.
static DepthUpdate make_depth(uint64_t U, uint64_t u,
                              std::vector<PriceLevel> bids = {},
                              std::vector<PriceLevel> asks = {}) {
    return DepthUpdate{"BTCUSDT", 0, U, u, std::move(bids), std::move(asks)};
}

// Helper: make a snapshot JSON response.
static std::string snapshot_json(uint64_t last_update_id,
                                 std::string bids = "[]",
                                 std::string asks = "[]") {
    return R"({"lastUpdateId":)" + std::to_string(last_update_id) +
           R"(,"bids":)" + bids + R"(,"asks":)" + asks + "}";
}

// Helper: SyncEngine config pointing at mock server.
static SyncEngine::Config mock_config(std::string const& port) {
    SyncEngine::Config cfg;
    cfg.snapshot_host = "testserver.local";
    cfg.snapshot_port = port;
    cfg.http_timeout = std::chrono::milliseconds(2000);
    cfg.base_retry_delay = std::chrono::milliseconds(10);
    return cfg;
}

BOOST_AUTO_TEST_CASE(initial_state_is_buffering) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    SyncEngine eng(ioc, ctx, SyncEngine::Config{});
    BOOST_TEST((eng.state() == SyncState::Buffering));
}

BOOST_AUTO_TEST_CASE(first_event_buffers_and_triggers_snapshot) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // Serve snapshot with lastUpdateId=100.
    srv.serve(snapshot_json(100, R"([["50.0","1.0"]])", R"([["51.0","2.0"]])"));

    auto cfg = mock_config(srv.port);
    SyncEngine eng(ioc, ctx, cfg);

    // First event: U=101, u=101 — triggers snapshot request.
    auto ev = eng.on_update(make_depth(101, 101, {{5000000000, 100000000}}, {}));
    BOOST_TEST((ev == SyncEvent::Buffered));

    ioc.run();

    // After snapshot is applied and buffered event processed:
    BOOST_TEST((eng.state() == SyncState::Synchronized));
    BOOST_TEST(eng.book().update_id() == 101u);
    BOOST_TEST(eng.book().bid_count() == 2u); // snapshot bid + update bid
}

BOOST_AUTO_TEST_CASE(stale_events_discarded_during_sync) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // Snapshot at 100.
    srv.serve(snapshot_json(100));

    auto cfg = mock_config(srv.port);
    SyncEngine eng(ioc, ctx, cfg);

    // Buffer events: some stale, one bridging, one after.
    eng.on_update(make_depth(95, 98));  // stale (u=98 <= 100)
    eng.on_update(make_depth(99, 101)); // bridging (U=99 <= 101, u=101 > 100)
    eng.on_update(make_depth(102, 103, {{500, 100}}, {})); // after

    ioc.run();

    BOOST_TEST((eng.state() == SyncState::Synchronized));
    BOOST_TEST(eng.book().update_id() == 103u);
    BOOST_TEST(eng.book().bid_count() == 1u);
}

BOOST_AUTO_TEST_CASE(gap_in_synchronized_state_triggers_resync) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    srv.serve(snapshot_json(100));

    auto cfg = mock_config(srv.port);
    SyncEngine eng(ioc, ctx, cfg);

    eng.on_update(make_depth(101, 101));
    ioc.run();
    BOOST_TEST((eng.state() == SyncState::Synchronized));
    BOOST_TEST(eng.book().update_id() == 101u);

    // Gap: expected U=102, got U=105.
    ioc.restart();
    auto ev = eng.on_update(make_depth(105, 106));
    BOOST_TEST((ev == SyncEvent::Resyncing));
    BOOST_TEST((eng.state() == SyncState::Buffering));
    BOOST_TEST(eng.book().update_id() == 0u);
}

BOOST_AUTO_TEST_CASE(sequential_updates_after_sync) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    srv.serve(snapshot_json(100));

    auto cfg = mock_config(srv.port);
    SyncEngine eng(ioc, ctx, cfg);

    eng.on_update(make_depth(101, 101));
    ioc.run();

    ioc.restart();
    auto ev1 = eng.on_update(make_depth(102, 103, {{500, 100}}, {{600, 200}}));
    BOOST_TEST((ev1 == SyncEvent::Applied));
    BOOST_TEST(eng.book().bid_count() == 1u);
    BOOST_TEST(eng.book().ask_count() == 1u);

    auto ev2 = eng.on_update(make_depth(104, 104, {{500, 0}}, {}));
    BOOST_TEST((ev2 == SyncEvent::Applied));
    BOOST_TEST(eng.book().bid_count() == 0u);
}

BOOST_AUTO_TEST_CASE(stale_snapshot_retries) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // First snapshot is stale (lastUpdateId=50 < first U=101).
    // Second snapshot is fresh (lastUpdateId=100).
    srv.serve(snapshot_json(50));

    auto cfg = mock_config(srv.port);
    SyncEngine eng(ioc, ctx, cfg);

    eng.on_update(make_depth(101, 101));
    ioc.run();

    // After stale snapshot, engine should have retried.
    // The retry timer will fire but the second snapshot request
    // needs a server. Serve it now.
    ioc.restart();
    srv.serve(snapshot_json(100));
    ioc.run();

    BOOST_TEST((eng.state() == SyncState::Synchronized));
    BOOST_TEST(eng.book().update_id() == 101u);
}

BOOST_AUTO_TEST_CASE(buffer_overflow_triggers_resync) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    SyncEngine::Config cfg;
    cfg.snapshot_host = "127.0.0.1";
    cfg.snapshot_port = "1"; // nothing listening
    cfg.http_timeout = std::chrono::milliseconds(100);
    SyncEngine eng(ioc, ctx, cfg);

    // Fill buffer to max.
    for (std::size_t i = 0; i < SyncEngine::kMaxBufferSize; ++i) {
        eng.on_update(make_depth(i + 1, i + 1));
    }

    // Next event overflows.
    auto ev = eng.on_update(make_depth(9999, 9999));
    BOOST_TEST((ev == SyncEvent::Resyncing));
    BOOST_TEST((eng.state() == SyncState::Buffering));
}

BOOST_AUTO_TEST_CASE(snapshot_http_error_triggers_resync) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // Return 500 error.
    srv.serve(R"({"error":"internal"})", 500);

    auto cfg = mock_config(srv.port);
    SyncEngine eng(ioc, ctx, cfg);

    eng.on_update(make_depth(101, 101));
    ioc.run();

    BOOST_TEST((eng.state() == SyncState::Buffering));
}

BOOST_AUTO_TEST_CASE(malformed_snapshot_triggers_resync) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    srv.serve("not json");

    auto cfg = mock_config(srv.port);
    SyncEngine eng(ioc, ctx, cfg);

    eng.on_update(make_depth(101, 101));
    ioc.run();

    BOOST_TEST((eng.state() == SyncState::Buffering));
}

BOOST_AUTO_TEST_CASE(no_bridging_event_triggers_resync) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // Snapshot at 100, but only buffered event has U=200 (huge gap).
    srv.serve(snapshot_json(100));

    auto cfg = mock_config(srv.port);
    SyncEngine eng(ioc, ctx, cfg);

    eng.on_update(make_depth(200, 201));
    ioc.run();

    BOOST_TEST((eng.state() == SyncState::Buffering));
}

BOOST_AUTO_TEST_CASE(stop_cancels_pending_work) {
    net::io_context ioc;
    MockServer srv(ioc);
    if (!srv.available) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind");
        return;
    }
    auto ctx = srv.make_client_ctx();

    // Server accepts but never responds.
    auto server_stream =
        std::make_shared<ssl::stream<tcp::socket>>(ioc, srv.server_ctx);
    srv.acceptor.async_accept(server_stream->next_layer(),
        [server_stream](beast::error_code ec) {
            if (ec) return;
            server_stream->async_handshake(ssl::stream_base::server,
                [](beast::error_code) {});
        });

    auto cfg = mock_config(srv.port);
    SyncEngine eng(ioc, ctx, cfg);

    eng.on_update(make_depth(101, 101));

    // Post stop after one tick.
    net::post(ioc, [&eng]() { eng.stop(); });

    ioc.run();
    // Should not hang — stop cancels the HTTP request.
}

BOOST_AUTO_TEST_SUITE_END()
