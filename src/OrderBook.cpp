#include "OrderBook.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lob {

namespace {

LevelSummary summarize(const PriceLevel& level) {
    return LevelSummary{level.price(), level.totalQuantity(), level.orderCount()};
}

template <typename Map>
std::vector<LevelSummary> topLevels(const Map& side, std::size_t n) {
    std::vector<LevelSummary> out;
    out.reserve(n < side.size() ? n : side.size());
    for (auto it = side.begin(); it != side.end() && out.size() < n; ++it) {
        out.push_back(summarize(it->second));
    }
    return out;
}

template <typename Map>
std::optional<LevelSummary> bestOf(const Map& side) {
    if (side.empty()) return std::nullopt;
    return summarize(side.begin()->second);  // maps are ordered best-first
}

}  // namespace

// ---- Mutations -----------------------------------------------------------

template <typename Map>
RejectReason OrderBook::addTo(Map& side, Order order) {
    const OrderId id = order.id();
    const Side which = order.side();
    const Price price = order.price();

    // Level is created first so that nothing below can leave an empty level
    // behind: any failure after this point rolls the insertion back.
    auto levelIt = side.find(price);
    if (levelIt == side.end()) levelIt = side.emplace(price, PriceLevel(price)).first;

    try {
        const PriceLevel::OrderHandle handle = levelIt->second.add(std::move(order));
        try {
            lookup_.emplace(id, OrderLocation{which, price, handle});
        } catch (...) {
            levelIt->second.remove(handle);  // undo; only touches the order just added
            throw;
        }
    } catch (...) {
        if (levelIt->second.empty()) side.erase(levelIt);
        throw;
    }
    return RejectReason::None;
}

RejectReason OrderBook::add(Order order) {
    if (lookup_.count(order.id()) != 0) return RejectReason::DuplicateOrderId;
    if (!order.isActive()) return RejectReason::OrderNotActive;
    if (order.remainingQuantity() <= 0) return RejectReason::InvalidQuantity;
    return order.side() == Side::Buy ? addTo(bids_, std::move(order))
                                     : addTo(asks_, std::move(order));
}

template <typename Map>
Order OrderBook::removeFrom(Map& side, Lookup::iterator entry) {
    const auto levelIt = side.find(entry->second.price);
    if (levelIt == side.end()) {
        throw std::logic_error("order book corrupted: lookup entry has no price level");
    }
    Order removed = levelIt->second.remove(entry->second.handle);
    lookup_.erase(entry);  // same step as the removal: no stale handle survives
    if (levelIt->second.empty()) side.erase(levelIt);
    return removed;
}

std::optional<Order> OrderBook::cancel(OrderId id) {
    const auto entry = lookup_.find(id);
    if (entry == lookup_.end()) return std::nullopt;
    Order removed = entry->second.side == Side::Buy ? removeFrom(bids_, entry)
                                                    : removeFrom(asks_, entry);
    removed.cancel();
    return removed;
}

// ---- Queries -------------------------------------------------------------

const Order* OrderBook::find(OrderId id) const {
    const auto entry = lookup_.find(id);
    // The handle is trustworthy by construction (see class comment).
    return entry == lookup_.end() ? nullptr : &*entry->second.handle;
}

std::optional<LevelSummary> OrderBook::bestBid() const { return bestOf(bids_); }
std::optional<LevelSummary> OrderBook::bestAsk() const { return bestOf(asks_); }

std::optional<Price> OrderBook::spread() const {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    return asks_.begin()->first - bids_.begin()->first;
}

std::optional<Midpoint> OrderBook::midPrice() const {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    const Price bid = bids_.begin()->first;
    const Price ask = asks_.begin()->first;
    // Prices are positive, so halving each and adding the carried halves is
    // exact and cannot overflow (bid + ask itself could).
    const Price oddBits = (bid % 2) + (ask % 2);  // 0, 1 or 2
    return Midpoint{bid / 2 + ask / 2 + oddBits / 2, oddBits == 1};
}

MarketDepth OrderBook::depth(std::size_t levels) const {
    return MarketDepth{topLevels(bids_, levels), topLevels(asks_, levels)};
}

// ---- Invariants ----------------------------------------------------------

template <typename Map>
void OrderBook::checkSide(const Map& side, Side which, std::vector<std::string>& out,
                          std::size_t& ordersSeen) const {
    const std::string name = which == Side::Buy ? "bid" : "ask";
    bool havePrev = false;
    Price prev = 0;

    for (const auto& [key, level] : side) {
        const std::string where = name + " level " + std::to_string(key);

        // (1) non-empty, (2/3) strictly best-first ordering, key matches level
        if (level.empty()) out.push_back(where + " is empty");
        if (havePrev && !side.key_comp()(prev, key)) {
            out.push_back(where + " is out of price order");
        }
        havePrev = true;
        prev = key;
        if (level.price() != key) out.push_back(where + " key differs from level price");

        Quantity sum = 0;
        std::size_t count = 0;
        bool haveSeq = false;
        Sequence lastSeq = 0;
        for (auto it = level.begin(); it != level.end(); ++it) {
            const Order& o = *it;
            ++count;
            ++ordersSeen;
            sum += o.remainingQuantity();
            const std::string who = where + ", order " + std::to_string(o.id());

            if (o.side() != which) out.push_back(who + " is on the wrong side");   // (4)
            if (o.price() != key) out.push_back(who + " price differs from level");  // (5)
            if (!o.isActive() || o.remainingQuantity() <= 0) {
                out.push_back(who + " is not an active order with remaining quantity");
            }
            // (10) FIFO: insertion order must agree with arrival sequence.
            if (haveSeq && o.sequence() < lastSeq) {
                out.push_back(who + " breaks FIFO (sequence went backwards)");
            }
            haveSeq = true;
            lastSeq = o.sequence();

            // (7/8) the lookup entry must exist and refer to exactly this node.
            // Handles are compared, never dereferenced.
            const auto entry = lookup_.find(o.id());
            if (entry == lookup_.end()) {
                out.push_back(who + " has no lookup entry");
            } else if (entry->second.side != which || entry->second.price != key ||
                       !(entry->second.handle == it)) {
                out.push_back(who + " lookup entry points elsewhere");
            }
        }
        // (9) aggregate matches the sum of remaining quantities
        if (sum != level.totalQuantity()) {
            out.push_back(where + " aggregate quantity " +
                          std::to_string(level.totalQuantity()) + " != sum " +
                          std::to_string(sum));
        }
        if (count != level.orderCount()) out.push_back(where + " order count mismatch");
        if (!level.checkInvariants()) out.push_back(where + " failed its own invariant check");
    }
}

InvariantReport OrderBook::checkInvariants() const {
    InvariantReport report;
    std::size_t ordersSeen = 0;
    checkSide(bids_, Side::Buy, report.violations, ordersSeen);
    checkSide(asks_, Side::Sell, report.violations, ordersSeen);

    // (6) ids globally unique across both sides.
    std::unordered_set<OrderId> ids;
    for (const auto& kv : bids_) {
        for (const Order& o : kv.second) {
            if (!ids.insert(o.id()).second) {
                report.violations.push_back("order id " + std::to_string(o.id()) +
                                            " appears more than once");
            }
        }
    }
    for (const auto& kv : asks_) {
        for (const Order& o : kv.second) {
            if (!ids.insert(o.id()).second) {
                report.violations.push_back("order id " + std::to_string(o.id()) +
                                            " appears more than once");
            }
        }
    }

    // (8, 11) Every resting order matched a lookup entry above (checkSide), and
    // ids are unique, so equal sizes prove there are no extra / stale entries.
    if (ordersSeen != lookup_.size()) {
        report.violations.push_back("lookup has " + std::to_string(lookup_.size()) +
                                    " entries but the book holds " +
                                    std::to_string(ordersSeen) + " orders");
    }
    return report;
}

}  // namespace lob
