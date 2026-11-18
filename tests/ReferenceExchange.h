#pragma once
// Test-only reference model of the whole exchange (matching + order lifecycle).
//
// It exists to check the production code independently, so it is deliberately
// different in structure:
//   * one flat std::map<id, Order> and linear scans: no price-level maps, no FIFO
//     lists, no handles, no lookup index;
//   * the best resting order is found by scanning every order and comparing
//     (price, arrival stamp), instead of walking ordered maps;
//   * book levels and the mid-price are derived on demand (the mid-price uses
//     unsigned 64-bit sums, not the production formula);
//   * nothing from MatchingEngine / OrderBook / PriceLevel / OrderManager is called.
// It only borrows the plain vocabulary types and enums from OrderTypes.h.
//
// It models the documented behaviour of the OrderManager API (see
// docs/ARCHITECTURE.md): id policy, validation order, execution at the resting
// price, FIFO by arrival, modification priority rules, terminal states.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "OrderTypes.h"

namespace ref {

using lob::OrderId;
using lob::Price;
using lob::Quantity;
using lob::Side;

struct Trade {
    lob::TradeId id = 0;
    OrderId maker = 0;
    OrderId taker = 0;
    Side takerSide = Side::Buy;
    Price price = 0;
    Quantity quantity = 0;
};

struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    Price originalPrice = 0;
    Price price = 0;
    Quantity submitted = 0;
    Quantity effective = 0;
    Quantity executed = 0;
    Quantity remaining = 0;
    Quantity cancelled = 0;
    lob::OrderStatus status = lob::OrderStatus::Rejected;
    lob::RejectReason rejectReason = lob::RejectReason::None;
    lob::CancelReason cancelReason = lob::CancelReason::None;
    std::uint64_t submissionStamp = 0;
    std::uint64_t stamp = 0;               // arrival stamp: smaller = older
    std::vector<std::size_t> tradeIndices;  // into Exchange::trades()
    bool active() const {
        return status == lob::OrderStatus::New || status == lob::OrderStatus::PartiallyFilled;
    }
};

// What the production report for the same operation must contain.
struct Expected {
    bool accepted = false;
    lob::RejectReason reason = lob::RejectReason::None;
    OrderId orderId = 0;
    lob::OrderStatus status = lob::OrderStatus::Rejected;
    lob::CancelReason cancelReason = lob::CancelReason::None;
    Quantity original = 0;
    Quantity executed = 0;
    Quantity remaining = 0;
    std::vector<Trade> trades;
};

struct Level {
    Price price = 0;
    Quantity total = 0;
    std::size_t count = 0;
};

struct Mid {
    std::uint64_t floorTicks = 0;
    bool half = false;
};

class Exchange {
public:
    explicit Exchange(OrderId firstGenerated = 1, lob::TradeId firstTrade = 1)
        : nextGenerated_(firstGenerated), nextTradeId_(firstTrade) {}

    // ---- Operations (each returns the expected production report) -----------------

    Expected submitGenerated(Side side, Price price, Quantity qty) {
        if (generatedExhausted_) return unrecorded(0, qty, lob::RejectReason::IdSpaceExhausted);
        OrderId id = 0;
        while (!generatedExhausted_) {
            const OrderId candidate = nextGenerated_;
            if (candidate == std::numeric_limits<OrderId>::max()) {
                generatedExhausted_ = true;
            } else {
                ++nextGenerated_;
            }
            if (orders_.count(candidate) == 0) {
                id = candidate;
                break;
            }
        }
        if (id == 0) return unrecorded(0, qty, lob::RejectReason::IdSpaceExhausted);
        return process(id, side, price, qty);
    }

    Expected submit(OrderId id, Side side, Price price, Quantity qty) {
        if (id == 0) return unrecorded(id, qty, lob::RejectReason::InvalidOrderId);
        if (orders_.count(id) != 0) return unrecorded(id, qty, lob::RejectReason::DuplicateOrderId);
        return process(id, side, price, qty);
    }

    Expected cancel(OrderId id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return unknownOrder(id);
        Order& o = it->second;
        if (!o.active()) return failure(o, lob::RejectReason::OrderNotActive);
        o.cancelled = o.remaining;
        o.remaining = 0;
        o.status = lob::OrderStatus::Cancelled;
        o.cancelReason = lob::CancelReason::UserRequested;
        Expected e = describe(o, true, lob::RejectReason::None);
        e.cancelReason = lob::CancelReason::UserRequested;
        return e;
    }

    Expected modify(OrderId id, std::optional<Price> newPrice, std::optional<Quantity> newQty) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return unknownOrder(id);
        Order& o = it->second;
        if (!o.active()) return failure(o, lob::RejectReason::OrderNotActive);
        if (newPrice && *newPrice <= 0) return failure(o, lob::RejectReason::InvalidPrice);
        if (newQty && *newQty <= 0) return failure(o, lob::RejectReason::InvalidQuantity);
        const Price price = newPrice.value_or(o.price);
        const Quantity qty = newQty.value_or(o.remaining);
        if (price == o.price && qty == o.remaining) return failure(o, lob::RejectReason::NoChange);
        if (price == o.price && qty < o.remaining) {  // in place: keeps its arrival stamp
            o.remaining = qty;
            o.effective = o.executed + qty;
            return describe(o, true, lob::RejectReason::None);
        }
        if (qty > std::numeric_limits<Quantity>::max() - o.executed) {
            return failure(o, lob::RejectReason::InvalidQuantity);
        }
        // Resting at the new price must not overflow that level's total. The old order's own
        // quantity does not count (it leaves its level). The sequence is consumed either way.
        if (activeTotalAt(o.side, price, o.id) > std::numeric_limits<Quantity>::max() - qty) {
            ++clock_;
            return failure(o, lob::RejectReason::QuantityOverflow);
        }
        // Priority lost: it re-enters as a fresh arrival at the new terms.
        o.price = price;
        o.remaining = qty;
        o.effective = o.executed + qty;
        o.stamp = ++clock_;
        Expected e;
        e.trades = matchIncoming(o);
        Expected out = describe(o, true, lob::RejectReason::None);
        out.trades = e.trades;
        return out;
    }

    // ---- Observation ---------------------------------------------------------------------

    const std::map<OrderId, Order>& orders() const { return orders_; }
    const std::vector<Trade>& trades() const { return trades_; }

    std::size_t activeCount() const {
        std::size_t n = 0;
        for (const auto& kv : orders_) n += kv.second.active() ? 1u : 0u;
        return n;
    }

    // Aggregated levels, best first: bids high to low, asks low to high.
    std::vector<Level> levels(Side side) const {
        std::map<Price, Level> byPrice;
        for (const auto& kv : orders_) {
            const Order& o = kv.second;
            if (!o.active() || o.side != side) continue;
            Level& l = byPrice[o.price];
            l.price = o.price;
            l.total += o.remaining;
            ++l.count;
        }
        std::vector<Level> out;
        for (const auto& kv : byPrice) out.push_back(kv.second);
        if (side == Side::Buy) std::reverse(out.begin(), out.end());
        return out;
    }

    std::optional<Price> bestPrice(Side side) const {
        const auto l = levels(side);
        if (l.empty()) return std::nullopt;
        return l.front().price;
    }

    // ask - bid, and (bid + ask) / 2 as floor + half-tick, via unsigned 64-bit sums
    // (both prices are positive int64, so the sum cannot overflow uint64).
    std::optional<Price> spread() const {
        const auto bid = bestPrice(Side::Buy);
        const auto ask = bestPrice(Side::Sell);
        if (!bid || !ask) return std::nullopt;
        return *ask - *bid;
    }
    std::optional<Mid> mid() const {
        const auto bid = bestPrice(Side::Buy);
        const auto ask = bestPrice(Side::Sell);
        if (!bid || !ask) return std::nullopt;
        const std::uint64_t sum = static_cast<std::uint64_t>(*bid) + static_cast<std::uint64_t>(*ask);
        return Mid{sum / 2, sum % 2 == 1};
    }

private:
    // Total remaining quantity of active orders on `side` at `price`, ignoring order `exclude`.
    Quantity activeTotalAt(Side side, Price price, OrderId exclude) const {
        Quantity total = 0;
        for (const auto& kv : orders_) {
            const Order& o = kv.second;
            if (o.active() && o.side == side && o.price == price && o.id != exclude) total += o.remaining;
        }
        return total;
    }

    Expected unrecorded(OrderId id, Quantity qty, lob::RejectReason reason) const {
        Expected e;
        e.accepted = false;
        e.reason = reason;
        e.orderId = id;
        e.status = lob::OrderStatus::Rejected;
        e.original = qty;
        return e;
    }
    Expected unknownOrder(OrderId id) const {
        Expected e;
        e.accepted = false;
        e.reason = lob::RejectReason::OrderNotFound;
        e.orderId = id;
        e.status = lob::OrderStatus::Rejected;
        return e;
    }
    static Expected describe(const Order& o, bool accepted, lob::RejectReason reason) {
        Expected e;
        e.accepted = accepted;
        e.reason = reason;
        e.orderId = o.id;
        e.status = o.status;
        e.original = o.effective;
        e.executed = o.executed;
        e.remaining = o.remaining;
        return e;
    }
    static Expected failure(const Order& o, lob::RejectReason reason) { return describe(o, false, reason); }

    Expected process(OrderId id, Side side, Price price, Quantity qty) {
        Order o;
        o.id = id;
        o.side = side;
        o.originalPrice = price;
        o.price = price;
        o.submitted = qty;
        o.effective = qty;
        if (price <= 0 || qty <= 0) {
            o.status = lob::OrderStatus::Rejected;
            o.rejectReason = price <= 0 ? lob::RejectReason::InvalidPrice : lob::RejectReason::InvalidQuantity;
            orders_[id] = o;
            return describe(orders_[id], false, o.rejectReason);
        }
        o.status = lob::OrderStatus::New;
        o.remaining = qty;
        o.stamp = ++clock_;
        o.submissionStamp = o.stamp;
        // The order's own side and price already hold too much for it to rest: rejected, and
        // (like production) it has consumed a sequence number and its id is retired.
        if (activeTotalAt(side, price, id) > std::numeric_limits<Quantity>::max() - qty) {
            o.status = lob::OrderStatus::Rejected;
            o.rejectReason = lob::RejectReason::QuantityOverflow;
            o.remaining = 0;
            orders_[id] = o;
            return describe(orders_[id], false, lob::RejectReason::QuantityOverflow);
        }
        orders_[id] = o;
        Order& stored = orders_[id];
        std::vector<Trade> made = matchIncoming(stored);
        Expected e = describe(stored, true, lob::RejectReason::None);
        e.trades = std::move(made);
        return e;
    }

    // Executes `incoming` (already stored in orders_, remaining > 0) against the best
    // eligible resting orders. Linear scan each round: best price, then oldest stamp.
    std::vector<Trade> matchIncoming(Order& incoming) {
        std::vector<Trade> made;
        while (incoming.remaining > 0) {
            Order* best = nullptr;
            for (auto& kv : orders_) {
                Order& o = kv.second;
                if (!o.active() || o.id == incoming.id || o.side == incoming.side) continue;
                const bool eligible = incoming.side == Side::Buy ? incoming.price >= o.price
                                                                 : incoming.price <= o.price;
                if (!eligible) continue;
                if (best == nullptr) {
                    best = &o;
                    continue;
                }
                const bool betterPrice = o.side == Side::Sell ? o.price < best->price : o.price > best->price;
                if (betterPrice || (o.price == best->price && o.stamp < best->stamp)) best = &o;
            }
            if (best == nullptr) break;
            const Quantity q = std::min(incoming.remaining, best->remaining);
            Trade t;
            t.id = nextTradeId_++;
            t.maker = best->id;
            t.taker = incoming.id;
            t.takerSide = incoming.side;
            t.price = best->price;  // the resting order's price
            t.quantity = q;
            trades_.push_back(t);
            made.push_back(t);
            for (Order* x : {&incoming, best}) {
                x->executed += q;
                x->remaining -= q;
                x->status = x->remaining == 0 ? lob::OrderStatus::Filled : lob::OrderStatus::PartiallyFilled;
                x->tradeIndices.push_back(trades_.size() - 1);
            }
        }
        return made;
    }

    std::map<OrderId, Order> orders_;
    std::vector<Trade> trades_;
    std::uint64_t clock_ = 0;
    OrderId nextGenerated_;
    bool generatedExhausted_ = false;
    lob::TradeId nextTradeId_;
};

}  // namespace ref
