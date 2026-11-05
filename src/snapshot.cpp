#include "snapshot.hpp"

#include <boost/json.hpp>

namespace json = boost::json;

SnapshotResult parse_snapshot(std::string_view data) {
    boost::system::error_code ec;
    auto jv = json::parse(data, ec);
    if (ec)
        return ParseError{"invalid JSON: " + ec.message()};

    if (!jv.is_object())
        return ParseError{"expected JSON object"};

    auto const& obj = jv.get_object();

    auto id_result = get_uint64(obj, "lastUpdateId");
    if (auto* err = std::get_if<ParseError>(&id_result))
        return *err;
    auto last_update_id = std::get<uint64_t>(id_result);

    // Bids (skip zero-quantity entries — they carry no book state).
    auto b_it = obj.find("bids");
    if (b_it == obj.end() || !b_it->value().is_array())
        return ParseError{"missing or non-array field \"bids\""};

    auto bids = parse_levels(b_it->value().get_array(), "bids", true);
    if (auto* err = std::get_if<ParseError>(&bids))
        return *err;

    // Asks.
    auto a_it = obj.find("asks");
    if (a_it == obj.end() || !a_it->value().is_array())
        return ParseError{"missing or non-array field \"asks\""};

    auto asks = parse_levels(a_it->value().get_array(), "asks", true);
    if (auto* err = std::get_if<ParseError>(&asks))
        return *err;

    return Snapshot{
        last_update_id,
        std::move(std::get<std::vector<PriceLevel>>(bids)),
        std::move(std::get<std::vector<PriceLevel>>(asks)),
    };
}
