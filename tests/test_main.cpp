#define BOOST_TEST_MODULE market_data_engine_tests
#include <boost/test/unit_test.hpp>

#include "depth_update.hpp"
#include "session.hpp"
#include "util.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <atomic>
#include <cstdint>
#include <limits>
#include <regex>
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
