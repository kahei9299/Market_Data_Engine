#include "replay.hpp"

#include <fstream>
#include <iostream>

std::vector<std::string> load_messages(std::string const& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    if (!in.is_open()) {
        std::cerr << "replay: cannot open " << path << '\n';
        return lines;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        // Strip optional timestamp prefix: "[2024-01-01T00:00:00.000Z] {...}"
        auto brace = line.find('{');
        if (brace != std::string::npos && brace > 0)
            line = line.substr(brace);
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<DepthUpdate> parse_messages(std::vector<std::string> const& raw,
                                        ReplayStats& stats) {
    std::vector<DepthUpdate> updates;
    updates.reserve(raw.size());
    for (auto const& msg : raw) {
        auto result = parse_depth_update(msg);
        if (auto* update = std::get_if<DepthUpdate>(&result)) {
            updates.push_back(std::move(*update));
            stats.messages_parsed++;
        } else {
            stats.parse_errors++;
        }
    }
    return updates;
}

ReplayResult replay_sequential(std::vector<DepthUpdate> const& updates,
                               uint64_t initial_update_id) {
    ReplayResult result;
    Snapshot init{initial_update_id, {}, {}};
    result.book.apply_snapshot(init);

    for (auto const& update : updates) {
        auto status = result.book.apply_update(update);
        switch (status) {
        case UpdateResult::Applied:
            result.stats.messages_applied++;
            break;
        case UpdateResult::Stale:
            result.stats.messages_stale++;
            break;
        case UpdateResult::Gap:
            result.stats.messages_gap++;
            result.stats.messages_parsed = updates.size();
            return result;
        }
    }
    result.stats.messages_parsed = updates.size();
    result.stats.complete = true;
    return result;
}
