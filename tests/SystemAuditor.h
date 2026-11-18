#pragma once
// Test-only system-level invariant auditor.
//
// It looks at an OrderManager only through its PUBLIC API (order(), tradesFor(),
// trades(), book(), recordedOrderCount(), isHistoryCurrent(), checkConsistency()),
// copies what it sees into a plain-data Snapshot, and checks invariants on that
// snapshot. Working on plain data means the audit itself is testable: the meta-tests
// corrupt snapshots and prove each class of violation is detected.
//
// Invariant codes in messages:
//   A  lifecycle consistency   B  quantity reconciliation   C  trade consistency
//   E  order book integrity    F  recovery / stale-history  T  transitions between snapshots
//
// Known limitation (no production API was added for tests): the manager cannot
// enumerate its records, so the caller passes the ids it has submitted; the audit
// checks that this set is complete (recordedOrderCount() equals its size).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "OrderManager.h"

namespace audit {

using lob::OrderId;
using lob::Price;
using lob::Quantity;

struct RestingOrder {
    OrderId id = 0;
    lob::Side side = lob::Side::Buy;
    Price price = 0;
    Quantity remaining = 0;
    Quantity executed = 0;
    Quantity original = 0;
    lob::Sequence sequence = 0;
    lob::OrderStatus status = lob::OrderStatus::New;
};

struct Level {
    Price price = 0;
    Quantity total = 0;       // as reported by the level
    std::size_t count = 0;    // as reported by the level
    std::vector<RestingOrder> orders;  // in FIFO order
};

struct Snapshot {
    bool current = true;  // OrderManager::isHistoryCurrent()
    std::size_t recorded = 0;
    std::size_t bookOrders = 0;
    std::size_t bidLevels = 0;
    std::size_t askLevels = 0;
    std::vector<lob::OrderRecord> records;           // one per tracked id
    std::vector<std::vector<lob::Trade>> tradesFor;  // parallel to records
    std::vector<lob::Trade> trades;
    std::vector<Level> bids;  // best first
    std::vector<Level> asks;  // best first
    std::optional<lob::LevelSummary> bestBid;
    std::optional<lob::LevelSummary> bestAsk;
    std::optional<Price> spread;
    std::optional<lob::Midpoint> mid;
    lob::MarketDepth depth1;
    lob::MarketDepth depth2;
};

inline std::vector<Level> levelsOf(const lob::OrderBook& book, bool bids) {
    std::vector<Level> out;
    auto take = [&out](const auto& side) {
        for (const auto& kv : side) {
            Level l;
            l.price = kv.first;
            l.total = kv.second.totalQuantity();
            l.count = kv.second.orderCount();
            for (const lob::Order& o : kv.second) {
                RestingOrder r;
                r.id = o.id();
                r.side = o.side();
                r.price = o.price();
                r.remaining = o.remainingQuantity();
                r.executed = o.executedQuantity();
                r.original = o.originalQuantity();
                r.sequence = o.sequence();
                r.status = o.status();
                l.orders.push_back(r);
            }
            out.push_back(std::move(l));
        }
    };
    if (bids) take(book.bids()); else take(book.asks());
    return out;
}

inline Snapshot take(const lob::OrderManager& m, const std::vector<OrderId>& ids) {
    Snapshot s;
    s.current = m.isHistoryCurrent();
    s.recorded = m.recordedOrderCount();
    s.trades = m.trades();
    const lob::OrderBook& book = m.book();
    s.bookOrders = book.orderCount();
    s.bidLevels = book.bidLevelCount();
    s.askLevels = book.askLevelCount();
    s.bids = levelsOf(book, true);
    s.asks = levelsOf(book, false);
    s.bestBid = book.bestBid();
    s.bestAsk = book.bestAsk();
    s.spread = book.spread();
    s.mid = book.midPrice();
    s.depth1 = book.depth(1);
    s.depth2 = book.depth(2);
    for (OrderId id : ids) {
        if (auto rec = m.order(id)) {
            s.records.push_back(*rec);
            s.tradesFor.push_back(m.tradesFor(id));
        } else {
            lob::OrderRecord missing;
            missing.id = 0;  // marks "no such record"
            s.records.push_back(missing);
            s.tradesFor.emplace_back();
        }
    }
    return s;
}

namespace detail {

struct Sink {
    std::vector<std::string>& out;
    void add(const char* code, const std::string& msg) { out.push_back(std::string("[") + code + "] " + msg); }
};

inline bool addChecked(Quantity a, Quantity b, Quantity& out) {
    if (b > 0 && a > std::numeric_limits<Quantity>::max() - b) return false;
    out = a + b;
    return true;
}

// A 128-bit unsigned accumulator (two 64-bit limbs). Sums of int64 quantities, such as
// "everything every order has executed", can legitimately exceed int64 when quantities
// are near the type maximum, and the auditor must not overflow while checking them.
struct Wide {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    void add(std::uint64_t v) {
        lo += v;
        if (lo < v) ++hi;  // carry
    }
    bool operator==(const Wide& o) const { return hi == o.hi && lo == o.lo; }
};

inline std::string idText(OrderId id) { return "order " + std::to_string(id); }

inline bool isTerminal(lob::OrderStatus s) { return lob::isTerminal(s); }

inline bool sameTrade(const lob::Trade& a, const lob::Trade& b) {
    return a.id() == b.id() && a.makerOrderId() == b.makerOrderId() && a.takerOrderId() == b.takerOrderId() &&
           a.takerSide() == b.takerSide() && a.price() == b.price() && a.quantity() == b.quantity() &&
           a.sequence() == b.sequence();
}

}  // namespace detail

// Checks one snapshot (and, if given, its relation to the previous one).
inline std::vector<std::string> check(const Snapshot& s, const Snapshot* previous = nullptr) {
    std::vector<std::string> violations;
    detail::Sink v{violations};
    using lob::OrderStatus;

    // ---------------- E: order book integrity (always valid, history or not) ----------------
    std::unordered_map<OrderId, std::vector<const RestingOrder*>> inBook;
    auto checkSide = [&](const std::vector<Level>& levels, bool bids) {
        const char* name = bids ? "bid" : "ask";
        for (std::size_t i = 0; i < levels.size(); ++i) {
            const Level& l = levels[i];
            const std::string where = std::string(name) + " level " + std::to_string(l.price);
            if (i > 0 && (bids ? !(levels[i - 1].price > l.price) : !(levels[i - 1].price < l.price))) {
                v.add("E1", where + " is out of order (bids must be strictly descending, asks ascending)");
            }
            if (l.orders.empty()) v.add("E2", where + " is empty but still in the book");
            if (l.count != l.orders.size()) v.add("E3", where + " order count differs from its orders");
            Quantity sum = 0;
            bool overflow = false;
            lob::Sequence lastSeq = 0;
            for (std::size_t k = 0; k < l.orders.size(); ++k) {
                const RestingOrder& o = l.orders[k];
                inBook[o.id].push_back(&o);
                if (!detail::addChecked(sum, o.remaining, sum)) overflow = true;
                if (o.side != (bids ? lob::Side::Buy : lob::Side::Sell)) v.add("E2", where + ", " + detail::idText(o.id) + " is on the wrong side");
                if (o.price != l.price) v.add("E2", where + ", " + detail::idText(o.id) + " has a different price");
                if (o.remaining <= 0) v.add("E2", where + ", " + detail::idText(o.id) + " has no remaining quantity");
                if (k > 0 && o.sequence < lastSeq) v.add("E2", where + ", " + detail::idText(o.id) + " breaks FIFO (sequence decreased)");
                lastSeq = o.sequence;
            }
            if (overflow) v.add("E3", where + " remaining quantities overflow int64");
            else if (sum != l.total) v.add("E3", where + " aggregate " + std::to_string(l.total) + " != sum of orders " + std::to_string(sum));
        }
    };
    checkSide(s.bids, true);
    checkSide(s.asks, false);
    if (s.bids.size() != s.bidLevels || s.asks.size() != s.askLevels) v.add("E5", "level counts disagree with the book's own counters");
    std::size_t counted = 0;
    for (const auto& kv : inBook) {
        counted += kv.second.size();
        if (kv.second.size() > 1) v.add("E4", detail::idText(kv.first) + " rests " + std::to_string(kv.second.size()) + " times");
    }
    if (counted != s.bookOrders) v.add("E5", "book.orderCount() " + std::to_string(s.bookOrders) + " != orders found in levels " + std::to_string(counted));

    // Best prices, spread, mid-price and depth agree with the levels.
    const std::optional<Price> bid = s.bids.empty() ? std::nullopt : std::optional<Price>(s.bids.front().price);
    const std::optional<Price> ask = s.asks.empty() ? std::nullopt : std::optional<Price>(s.asks.front().price);
    if (bid.has_value() != s.bestBid.has_value() || (bid && s.bestBid && *bid != s.bestBid->price)) v.add("E6", "bestBid() differs from the first bid level");
    if (ask.has_value() != s.bestAsk.has_value() || (ask && s.bestAsk && *ask != s.bestAsk->price)) v.add("E6", "bestAsk() differs from the first ask level");
    if (s.bestBid && !s.bids.empty() && (s.bestBid->totalQuantity != s.bids.front().total || s.bestBid->orderCount != s.bids.front().count)) v.add("E6", "bestBid() summary differs from its level");
    if (s.bestAsk && !s.asks.empty() && (s.bestAsk->totalQuantity != s.asks.front().total || s.bestAsk->orderCount != s.asks.front().count)) v.add("E6", "bestAsk() summary differs from its level");
    if (bid && ask) {
        if (!(*bid < *ask)) v.add("D1", "the book is crossed: best bid " + std::to_string(*bid) + " >= best ask " + std::to_string(*ask));
        if (!s.spread || *s.spread != *ask - *bid) v.add("E6", "spread() != best ask - best bid");
        const std::uint64_t sum = static_cast<std::uint64_t>(*bid) + static_cast<std::uint64_t>(*ask);
        if (!s.mid || static_cast<std::uint64_t>(s.mid->ticks) != sum / 2 || s.mid->halfTick != (sum % 2 == 1)) v.add("E6", "midPrice() is not (bid + ask) / 2");
    } else {
        if (s.spread || s.mid) v.add("E6", "spread/mid-price reported with an empty side");
    }
    auto depthOk = [&](const lob::MarketDepth& d, std::size_t n) {
        const std::size_t nb = std::min(n, s.bids.size());
        const std::size_t na = std::min(n, s.asks.size());
        if (d.bids.size() != nb || d.asks.size() != na) return false;
        for (std::size_t i = 0; i < nb; ++i) {
            if (d.bids[i].price != s.bids[i].price || d.bids[i].totalQuantity != s.bids[i].total || d.bids[i].orderCount != s.bids[i].count) return false;
        }
        for (std::size_t i = 0; i < na; ++i) {
            if (d.asks[i].price != s.asks[i].price || d.asks[i].totalQuantity != s.asks[i].total || d.asks[i].orderCount != s.asks[i].count) return false;
        }
        return true;
    };
    if (!depthOk(s.depth1, 1) || !depthOk(s.depth2, 2)) v.add("E6", "depth(n) differs from the book levels");

    // ---------------- Record-level checks (B, A7): valid with or without current history ----------------
    std::map<OrderId, std::size_t> recordIndex;
    for (std::size_t i = 0; i < s.records.size(); ++i) {
        const lob::OrderRecord& r = s.records[i];
        if (r.id == 0) {
            v.add("A5", "a tracked order id has no lifecycle record");
            continue;
        }
        if (!recordIndex.emplace(r.id, i).second) v.add("A5", detail::idText(r.id) + " is tracked twice");
        const std::string who = detail::idText(r.id);
        if (r.executedQuantity < 0 || r.remainingQuantity < 0 || r.cancelledQuantity < 0) v.add("B2", who + " has a negative quantity");
        if (r.status != OrderStatus::Rejected) {
            Quantity sum = 0;
            if (!detail::addChecked(r.executedQuantity, r.remainingQuantity, sum) || !detail::addChecked(sum, r.cancelledQuantity, sum)) {
                v.add("B1", who + " quantities overflow int64");
            } else if (sum != r.effectiveQuantity) {
                v.add("B1", who + " executed " + std::to_string(r.executedQuantity) + " + remaining " + std::to_string(r.remainingQuantity) +
                                " + cancelled " + std::to_string(r.cancelledQuantity) + " != effective " + std::to_string(r.effectiveQuantity));
            }
        }
        switch (r.status) {
            case OrderStatus::New:
                if (r.executedQuantity != 0 || r.remainingQuantity <= 0 || r.cancelledQuantity != 0) v.add("B3", who + " is NEW but has executions, no remaining quantity, or a cancelled quantity");
                break;
            case OrderStatus::PartiallyFilled:
                if (r.executedQuantity <= 0 || r.remainingQuantity <= 0 || r.cancelledQuantity != 0) v.add("B3", who + " is PARTIALLY_FILLED without both executed and remaining quantity");
                break;
            case OrderStatus::Filled:
                if (r.remainingQuantity != 0 || r.cancelledQuantity != 0 || r.executedQuantity != r.effectiveQuantity || r.executedQuantity <= 0) v.add("B3", who + " is FILLED but not fully executed");
                break;
            case OrderStatus::Cancelled:
                if (r.remainingQuantity != 0 || r.cancelledQuantity <= 0 || r.cancelReason == lob::CancelReason::None) v.add("B3", who + " is CANCELLED without a cancelled quantity / reason, or still has remaining quantity");
                break;
            case OrderStatus::Rejected:
                if (r.executedQuantity != 0 || r.remainingQuantity != 0 || r.cancelledQuantity != 0 || !r.tradeIndices.empty() || r.rejectReason == lob::RejectReason::None) v.add("A7", who + " is REJECTED but has executions, quantity, trades or no reason");
                break;
        }
    }

    // ---------------- A: lifecycle <-> book (only meaningful when history is current) ----------------
    if (s.current) {
        std::size_t activeRecords = 0;
        for (const lob::OrderRecord& r : s.records) {
            if (r.id == 0) continue;
            const auto it = inBook.find(r.id);
            const std::size_t occurrences = it == inBook.end() ? 0 : it->second.size();
            if (r.isActive()) {
                ++activeRecords;
                if (occurrences != 1) {
                    v.add("A1", detail::idText(r.id) + " is ACTIVE but rests " + std::to_string(occurrences) + " times in the book");
                } else {
                    const RestingOrder& o = *it->second.front();
                    if (o.side != r.side || o.price != r.price || o.remaining != r.remainingQuantity || o.executed != r.executedQuantity ||
                        o.original != r.effectiveQuantity || o.sequence != r.sequence || o.status != r.status) {
                        v.add("A1", detail::idText(r.id) + " differs between its lifecycle record and the resting order");
                    }
                }
            } else if (occurrences != 0) {
                v.add("A3", detail::idText(r.id) + " is terminal (" + lob::toString(r.status) + ") but still rests in the book");
            }
        }
        for (const auto& kv : inBook) {
            const auto rec = recordIndex.find(kv.first);
            if (rec == recordIndex.end() || !s.records[rec->second].isActive()) v.add("A2", detail::idText(kv.first) + " rests in the book without an ACTIVE lifecycle record");
        }
        if (activeRecords != s.bookOrders) v.add("A1", "active records (" + std::to_string(activeRecords) + ") != orders in the book (" + std::to_string(s.bookOrders) + ")");
        if (s.recorded != s.records.size()) v.add("A5", "recordedOrderCount() " + std::to_string(s.recorded) + " != tracked ids " + std::to_string(s.records.size()));
    }

    // ---------------- C: trades ----------------
    std::map<lob::TradeId, std::size_t> tradeIndexById;
    lob::TradeId lastId = 0;
    for (std::size_t i = 0; i < s.trades.size(); ++i) {
        const lob::Trade& t = s.trades[i];
        const std::string what = "trade " + std::to_string(t.id());
        if (t.quantity() <= 0) v.add("C1", what + " has non-positive quantity");
        if (t.price() <= 0) v.add("C1", what + " has non-positive price");
        if (t.makerOrderId() == 0 || t.takerOrderId() == 0 || t.buyOrderId() == 0 || t.sellOrderId() == 0) v.add("C1", what + " has an invalid order id");
        if (t.buyOrderId() == t.sellOrderId()) v.add("C1", what + " has the same buyer and seller");
        if (!tradeIndexById.emplace(t.id(), i).second) v.add("C4", what + " id appears twice");
        if (i > 0 && t.id() <= lastId) v.add("C4", what + " id is not increasing");
        lastId = t.id();
        for (OrderId party : {t.buyOrderId(), t.sellOrderId()}) {
            if (recordIndex.find(party) == recordIndex.end()) v.add("C1", what + " refers to untracked " + detail::idText(party));
        }
    }
    // Expected trade list of each tracked order, derived from the global list alone.
    std::map<OrderId, std::vector<lob::TradeId>> expected;
    detail::Wide totalTraded;  // 128-bit: totals of int64 quantities can exceed int64
    for (const lob::Trade& t : s.trades) {
        expected[t.buyOrderId()].push_back(t.id());
        expected[t.sellOrderId()].push_back(t.id());

        totalTraded.add(static_cast<std::uint64_t>(t.quantity()));
    }
    std::size_t listed = 0;
    for (std::size_t i = 0; i < s.records.size(); ++i) {
        const lob::OrderRecord& r = s.records[i];
        if (r.id == 0) continue;
        const std::string who = detail::idText(r.id);
        const std::vector<lob::Trade>& mine = s.tradesFor[i];
        listed += mine.size();
        Quantity executedFromList = 0;
        std::set<lob::TradeId> seen;
        lob::TradeId prevId = 0;
        for (const lob::Trade& t : mine) {
            if (!seen.insert(t.id()).second) v.add("C2", who + " lists trade " + std::to_string(t.id()) + " twice");
            if (t.id() <= prevId) v.add("C5", who + " trade history is not in execution order");
            prevId = t.id();
            if (t.buyOrderId() != r.id && t.sellOrderId() != r.id) v.add("C2", who + " lists trade " + std::to_string(t.id()) + " that it was not part of");
            const auto g = tradeIndexById.find(t.id());
            if (g == tradeIndexById.end() || !detail::sameTrade(s.trades[g->second], t)) v.add("C2", who + " lists a trade that is not in the global trade list");
            Quantity next = 0;
            if (detail::addChecked(executedFromList, t.quantity(), next)) executedFromList = next;
        }
        if (executedFromList != r.executedQuantity) v.add("C3", who + " executed " + std::to_string(r.executedQuantity) + " != sum of its trades " + std::to_string(executedFromList));
        if (r.tradeIndices.size() != mine.size()) v.add("C2", who + " tradeIndices size differs from tradesFor()");
        const std::vector<lob::TradeId>& want = expected[r.id];
        if (s.current) {
            // Exactly once as buyer or seller, in order, nothing missing.
            bool same = want.size() == mine.size();
            for (std::size_t k = 0; same && k < want.size(); ++k) same = want[k] == mine[k].id();
            if (!same) v.add("C2", who + " trade history (" + std::to_string(mine.size()) + ") differs from the global list (" + std::to_string(want.size()) + ")");
        } else {
            // Mid-recovery: what has been applied is an in-order prefix of what will be.
            bool prefix = mine.size() <= want.size();
            for (std::size_t k = 0; prefix && k < mine.size(); ++k) prefix = want[k] == mine[k].id();
            if (!prefix) v.add("C2", who + " applied trade history is not a prefix of its eventual history");
        }
    }
    if (s.current && listed != 2 * s.trades.size()) v.add("C2", "sum of per-order trade counts " + std::to_string(listed) + " != 2 x trades " + std::to_string(s.trades.size()));
    if (s.current) {
        detail::Wide executedSum;
        for (const lob::OrderRecord& r : s.records) {
            if (r.id != 0 && r.executedQuantity >= 0) executedSum.add(static_cast<std::uint64_t>(r.executedQuantity));
        }
        detail::Wide twiceTraded = totalTraded;
        twiceTraded.add(totalTraded.lo);
        twiceTraded.hi += totalTraded.hi;
        if (!(executedSum == twiceTraded)) v.add("C3", "total executed over orders != 2 x total traded");
    }

    // ---------------- T: transitions since the previous snapshot ----------------
    if (previous != nullptr) {
        std::map<OrderId, const lob::OrderRecord*> before;
        std::map<OrderId, const std::vector<lob::Trade>*> beforeTrades;
        for (std::size_t i = 0; i < previous->records.size(); ++i) {
            if (previous->records[i].id != 0) {
                before[previous->records[i].id] = &previous->records[i];
                beforeTrades[previous->records[i].id] = &previous->tradesFor[i];
            }
        }
        if (s.trades.size() < previous->trades.size()) {
            v.add("T1", "the global trade list shrank");
        } else {
            for (std::size_t i = 0; i < previous->trades.size(); ++i) {
                if (!detail::sameTrade(previous->trades[i], s.trades[i])) { v.add("T1", "an already-committed trade changed or moved"); break; }
            }
        }
        if (s.recorded < previous->recorded) v.add("T2", "recordedOrderCount() decreased (an id was forgotten, so it could be reused)");
        for (std::size_t i = 0; i < s.records.size(); ++i) {
            const lob::OrderRecord& now = s.records[i];
            if (now.id == 0) continue;
            const auto it = before.find(now.id);
            if (it == before.end()) continue;
            const lob::OrderRecord& was = *it->second;
            const std::string who = detail::idText(now.id);
            if (now.side != was.side || now.originalPrice != was.originalPrice || now.submissionSequence != was.submissionSequence || now.submittedQuantity != was.submittedQuantity) {
                v.add("A6", who + " was re-created or altered: its immutable fields changed (id reuse?)");
            }
            if (now.executedQuantity < was.executedQuantity) v.add("B4", who + " executed quantity decreased from " + std::to_string(was.executedQuantity) + " to " + std::to_string(now.executedQuantity));
            if (now.sequence < was.sequence) v.add("T3", who + " sequence decreased");
            if (detail::isTerminal(was.status)) {
                if (now.status != was.status || now.executedQuantity != was.executedQuantity || now.remainingQuantity != was.remainingQuantity ||
                    now.cancelledQuantity != was.cancelledQuantity || now.effectiveQuantity != was.effectiveQuantity || now.rejectReason != was.rejectReason ||
                    now.cancelReason != was.cancelReason || now.tradeIndices != was.tradeIndices) {
                    v.add("A6", who + " was terminal (" + lob::toString(was.status) + ") but changed afterwards");
                }
            } else if (!lob::canTransition(was.status, now.status) && now.status != was.status) {
                v.add("T3", who + " made an illegal transition " + lob::toString(was.status) + " -> " + lob::toString(now.status));
            }
            const std::vector<lob::Trade>& beforeList = *beforeTrades[now.id];
            const std::vector<lob::Trade>& nowList = s.tradesFor[i];
            bool prefix = beforeList.size() <= nowList.size();
            for (std::size_t k = 0; prefix && k < beforeList.size(); ++k) prefix = detail::sameTrade(beforeList[k], nowList[k]);
            if (!prefix) v.add("T4", who + " trade history lost or reordered an earlier trade");
        }
    }
    return violations;
}

// Takes a snapshot and checks it against the previous one (kept internally), and
// cross-checks the production consistency audit and the stale-history flag.
class Auditor {
public:
    std::vector<std::string> audit(const lob::OrderManager& m, const std::vector<OrderId>& ids) {
        Snapshot now = take(m, ids);
        std::vector<std::string> out = check(now, have_ ? &previous_ : nullptr);

        // Production's own checks must agree with this independent audit.
        const lob::InvariantReport book = m.book().checkInvariants();
        for (const auto& msg : book.violations) out.push_back("[E0] OrderBook::checkInvariants: " + msg);
        const lob::InvariantReport mgr = m.checkConsistency();
        if (m.isHistoryCurrent()) {
            for (const auto& msg : mgr.violations) out.push_back("[F1] OrderManager::checkConsistency: " + msg);
        } else if (mgr.violations.size() != 1 || mgr.violations[0] != lob::kHistoryNotCurrent) {
            out.push_back("[F2] history is stale but checkConsistency() did not report exactly the stale notice");
        }
        previous_ = std::move(now);
        have_ = true;
        return out;
    }
    const Snapshot& last() const { return previous_; }

private:
    Snapshot previous_;
    bool have_ = false;
};

}  // namespace audit
