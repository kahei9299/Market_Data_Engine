#include "depth_update.hpp"
#include "util.hpp"

#include <boost/json.hpp>

namespace json = boost::json;

std::variant<uint64_t, ParseError>
get_uint64(json::object const& obj, char const* key) {
    auto it = obj.find(key);
    if (it == obj.end())
        return ParseError{"missing field \"" + std::string(key) + "\""};

    auto const& val = it->value();
    if (!val.is_int64() && !val.is_uint64())
        return ParseError{"field \"" + std::string(key) + "\" is not an integer"};

    if (val.is_int64()) {
        auto v = val.get_int64();
        if (v < 0)
            return ParseError{"field \"" + std::string(key) + "\" is negative"};
        return static_cast<uint64_t>(v);
    }
    return val.get_uint64();
}

std::variant<std::vector<PriceLevel>, ParseError>
parse_levels(json::array const& arr, char const* side, bool skip_zero_qty) {
    std::vector<PriceLevel> levels;
    levels.reserve(arr.size());

    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (!arr[i].is_array())
            return ParseError{std::string(side) + "[" + std::to_string(i) +
                              "]: expected array"};

        auto const& entry = arr[i].get_array();
        if (entry.size() != 2)
            return ParseError{std::string(side) + "[" + std::to_string(i) +
                              "]: expected 2 elements, got " +
                              std::to_string(entry.size())};

        if (!entry[0].is_string() || !entry[1].is_string())
            return ParseError{std::string(side) + "[" + std::to_string(i) +
                              "]: price and quantity must be strings"};

        auto price_str = entry[0].get_string();
        auto qty_str = entry[1].get_string();

        auto price_result = parse_scaled_decimal(
            std::string_view(price_str.data(), price_str.size()));
        if (auto* err = std::get_if<ScaledParseError>(&price_result))
            return ParseError{std::string(side) + "[" + std::to_string(i) +
                              "].price: " + err->message};

        auto qty_result = parse_scaled_decimal(
            std::string_view(qty_str.data(), qty_str.size()));
        if (auto* err = std::get_if<ScaledParseError>(&qty_result))
            return ParseError{std::string(side) + "[" + std::to_string(i) +
                              "].quantity: " + err->message};

        auto price = std::get<int64_t>(price_result);
        auto qty = std::get<int64_t>(qty_result);

        if (price <= 0)
            return ParseError{std::string(side) + "[" + std::to_string(i) +
                              "]: price must be positive"};

        if (qty < 0)
            return ParseError{std::string(side) + "[" + std::to_string(i) +
                              "]: quantity must be nonnegative"};

        if (skip_zero_qty && qty == 0)
            continue;

        levels.push_back(PriceLevel{price, qty});
    }

    return levels;
}

ParseResult parse_depth_update(std::string_view data) {
    boost::system::error_code ec;
    auto jv = json::parse(data, ec);
    if (ec)
        return ParseError{"invalid JSON: " + ec.message()};

    if (!jv.is_object())
        return ParseError{"expected JSON object"};

    auto const& obj = jv.get_object();

    // Event type.
    auto e_it = obj.find("e");
    if (e_it == obj.end() || !e_it->value().is_string())
        return ParseError{"missing or non-string field \"e\""};
    if (e_it->value().get_string() != "depthUpdate")
        return ParseError{"unexpected event type: " +
                          std::string(e_it->value().get_string())};

    // Symbol.
    auto s_it = obj.find("s");
    if (s_it == obj.end() || !s_it->value().is_string())
        return ParseError{"missing or non-string field \"s\""};
    if (s_it->value().get_string().empty())
        return ParseError{"empty symbol"};

    // Timestamps and sequence IDs.
    auto event_time = get_uint64(obj, "E");
    if (auto* err = std::get_if<ParseError>(&event_time))
        return *err;

    auto first_id = get_uint64(obj, "U");
    if (auto* err = std::get_if<ParseError>(&first_id))
        return *err;

    auto last_id = get_uint64(obj, "u");
    if (auto* err = std::get_if<ParseError>(&last_id))
        return *err;

    auto U = std::get<uint64_t>(first_id);
    auto u = std::get<uint64_t>(last_id);
    if (U > u)
        return ParseError{"invalid sequence range: U(" + std::to_string(U) +
                          ") > u(" + std::to_string(u) + ")"};

    // Bids.
    auto b_it = obj.find("b");
    if (b_it == obj.end() || !b_it->value().is_array())
        return ParseError{"missing or non-array field \"b\""};

    auto bids = parse_levels(b_it->value().get_array(), "bids");
    if (auto* err = std::get_if<ParseError>(&bids))
        return *err;

    // Asks.
    auto a_it = obj.find("a");
    if (a_it == obj.end() || !a_it->value().is_array())
        return ParseError{"missing or non-array field \"a\""};

    auto asks = parse_levels(a_it->value().get_array(), "asks");
    if (auto* err = std::get_if<ParseError>(&asks))
        return *err;

    return DepthUpdate{
        std::string(s_it->value().get_string()),
        std::get<uint64_t>(event_time),
        U,
        u,
        std::move(std::get<std::vector<PriceLevel>>(bids)),
        std::move(std::get<std::vector<PriceLevel>>(asks)),
    };
}
