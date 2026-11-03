#include "validation.hpp"

#include <boost/json.hpp>

namespace json = boost::json;

ValidationResult validate_depth_message(std::string_view data) {
    boost::system::error_code ec;
    auto jv = json::parse(data, ec);
    if (ec)
        return {false, "invalid JSON: " + ec.message()};

    if (!jv.is_object())
        return {false, "expected JSON object"};

    auto const& obj = jv.get_object();

    auto it = obj.find("e");
    if (it == obj.end() || !it->value().is_string())
        return {false, "missing or non-string field \"e\""};
    if (it->value().get_string() != "depthUpdate")
        return {false, "unexpected event type: " + std::string(it->value().get_string())};

    for (auto const* key : {"E", "U", "u"}) {
        auto f = obj.find(key);
        if (f == obj.end() || !f->value().is_number())
            return {false, std::string("missing or non-numeric field \"") + key + "\""};
    }

    for (auto const* key : {"b", "a"}) {
        auto f = obj.find(key);
        if (f == obj.end() || !f->value().is_array())
            return {false, std::string("missing or non-array field \"") + key + "\""};
    }

    return {true, {}};
}
