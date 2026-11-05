#include "order_book.hpp"
#include "util.hpp"

#include <iostream>

namespace {

void apply_levels(std::map<int64_t, int64_t>& side,
                  std::vector<PriceLevel> const& levels) {
    for (auto const& level : levels) {
        if (level.quantity == 0)
            side.erase(level.price);
        else
            side[level.price] = level.quantity;
    }
}

} // namespace

void OrderBook::apply_snapshot(Snapshot const& snap) {
    bids_.clear();
    asks_.clear();

    apply_levels(bids_, snap.bids);
    apply_levels(asks_, snap.asks);

    update_id_ = snap.last_update_id;
}

UpdateResult OrderBook::apply_update(DepthUpdate const& update) {
    if (update.last_update_id <= update_id_)
        return UpdateResult::Stale;

    // Guard against unsigned overflow: if update_id_ == UINT64_MAX,
    // update_id_ + 1 wraps to 0, which would accept any U.
    if (update_id_ == UINT64_MAX || update.first_update_id > update_id_ + 1)
        return UpdateResult::Gap;

    apply_levels(bids_, update.bids);
    apply_levels(asks_, update.asks);
    update_id_ = update.last_update_id;

    // Warn on crossed book (best_bid > best_ask). Locked book is not crossed.
    if (!bids_.empty() && !asks_.empty()) {
        auto bb = bids_.rbegin()->first;
        auto ba = asks_.begin()->first;
        if (bb > ba) {
            std::cerr << "warning: crossed book (best_bid="
                      << format_scaled(bb) << " > best_ask="
                      << format_scaled(ba) << ")\n";
        }
    }

    return UpdateResult::Applied;
}

std::optional<std::pair<int64_t, int64_t>> OrderBook::best_bid() const {
    if (bids_.empty())
        return std::nullopt;
    auto it = bids_.rbegin();
    return std::pair{it->first, it->second};
}

std::optional<std::pair<int64_t, int64_t>> OrderBook::best_ask() const {
    if (asks_.empty())
        return std::nullopt;
    auto it = asks_.begin();
    return std::pair{it->first, it->second};
}
