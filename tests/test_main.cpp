#define BOOST_TEST_MODULE market_data_engine_tests
#include <boost/test/unit_test.hpp>

#include "util.hpp"
#include "validation.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <regex>
#include <string>

// --- timestamp_now tests ---

BOOST_AUTO_TEST_SUITE(timestamp)

BOOST_AUTO_TEST_CASE(format_matches_iso8601) {
    auto ts = timestamp_now();
    // e.g. 2026-11-02T04:08:46.162Z
    std::regex pattern(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)");
    BOOST_TEST(std::regex_match(ts, pattern));
}

BOOST_AUTO_TEST_CASE(length_is_24) {
    auto ts = timestamp_now();
    BOOST_TEST(ts.size() == 24u);
}

BOOST_AUTO_TEST_SUITE_END()

// --- validate_depth_message tests ---

BOOST_AUTO_TEST_SUITE(validation)

static const std::string kValidMessage = R"({
    "e": "depthUpdate",
    "E": 1793587539014,
    "s": "BTCUSDT",
    "U": 102171391360,
    "u": 102171391511,
    "b": [["78450.06000000","0.04581000"]],
    "a": [["78450.07000000","2.27294000"]]
})";

BOOST_AUTO_TEST_CASE(valid_message) {
    auto r = validate_depth_message(kValidMessage);
    BOOST_TEST(r.ok);
    BOOST_TEST(r.error.empty());
}

BOOST_AUTO_TEST_CASE(invalid_json) {
    auto r = validate_depth_message("not json at all");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("invalid JSON") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(empty_input) {
    auto r = validate_depth_message("");
    BOOST_TEST(!r.ok);
}

BOOST_AUTO_TEST_CASE(json_array_not_object) {
    auto r = validate_depth_message("[1, 2, 3]");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("expected JSON object") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_event_type) {
    auto r = validate_depth_message(R"({
        "E": 123, "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("\"e\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(wrong_event_type) {
    auto r = validate_depth_message(R"({
        "e": "trade", "E": 123, "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("unexpected event type") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_numeric_field_U) {
    auto r = validate_depth_message(R"({
        "e": "depthUpdate", "E": 123, "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("\"U\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_bids_array) {
    auto r = validate_depth_message(R"({
        "e": "depthUpdate", "E": 123, "U": 1, "u": 2, "a": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("\"b\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_asks_array) {
    auto r = validate_depth_message(R"({
        "e": "depthUpdate", "E": 123, "U": 1, "u": 2, "b": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("\"a\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(bids_not_array) {
    auto r = validate_depth_message(R"({
        "e": "depthUpdate", "E": 123, "U": 1, "u": 2, "b": "wrong", "a": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("\"b\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(empty_arrays_are_valid) {
    auto r = validate_depth_message(R"({
        "e": "depthUpdate", "E": 123, "s": "BTCUSDT", "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(r.ok);
}

BOOST_AUTO_TEST_SUITE_END()

// --- bounded buffer test ---

BOOST_AUTO_TEST_SUITE(buffer)

BOOST_AUTO_TEST_CASE(flat_buffer_respects_max_size) {
    boost::beast::flat_buffer buf(64);
    BOOST_TEST(buf.max_size() == 64u);
}

BOOST_AUTO_TEST_SUITE_END()
