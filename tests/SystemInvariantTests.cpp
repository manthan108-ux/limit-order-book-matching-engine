// Meta-tests for the system auditor (SystemAuditor.h): a healthy system passes, and
// each class of corruption is detected. Without these, an auditor that always says
// "all good" would look identical to one that works.
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "Duel.h"
#include "SystemAuditor.h"
#include "TestFramework.h"

using namespace lob;

namespace {

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

// A deterministic scenario that exercises every state: resting, partial, filled,
// cancelled, modified (in place and with priority loss) and rejected orders.
std::unique_ptr<Duel> scenario() {
    auto d = std::make_unique<Duel>("scenario");
    d->submit(1, S, 100, 10);
    d->submit(2, S, 101, 10);
    d->submit(3, S, 101, 5);
    d->submit(4, B, 99, 7);
    d->submit(5, B, 98, 3);
    d->submit(6, B, 101, 14);   // fills 1, partially fills 2
    d->submit(7, S, 99, 20);    // fills 4, rests 13 at 99
    d->cancel(3);
    d->modify(5, std::nullopt, 1);  // in place
    d->modify(2, 102, std::nullopt);  // loses priority
    d->submit(8, B, 97, 2);     // a second bid level
    d->submit(9, B, 0, 10);     // rejected record
    d->checkEverything("scenario setup");
    return d;
}

bool hasCode(const std::vector<std::string>& violations, const std::string& code) {
    for (const auto& v : violations) {
        if (v.rfind("[" + code + "]", 0) == 0) return true;
    }
    return false;
}

std::string dump(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += "\n      " + s;
    return out;
}

// Finds the record of `id` in a snapshot.
audit::Snapshot snap(Duel& d) { return audit::take(d.manager, d.ids); }

std::size_t indexOf(const audit::Snapshot& s, OrderId id) {
    for (std::size_t i = 0; i < s.records.size(); ++i) {
        if (s.records[i].id == id) return i;
    }
    return s.records.size();
}

}  // namespace

TEST(AuditorAcceptsAHealthySystem) {
    auto d = scenario();
    CHECK(d->clean());
    const audit::Snapshot s = snap(*d);
    const auto v = audit::check(s);
    CHECK_MSG(v.empty(), dump(v));
    // The scenario really covers the states it claims to.
    CHECK(s.records[indexOf(s, 1)].status == OrderStatus::Filled);
    CHECK(s.records[indexOf(s, 2)].status == OrderStatus::PartiallyFilled);
    CHECK(s.records[indexOf(s, 3)].status == OrderStatus::Cancelled);
    CHECK(s.records[indexOf(s, 9)].status == OrderStatus::Rejected);
    CHECK(!s.trades.empty());
    CHECK(!s.bids.empty());
    CHECK(!s.asks.empty());
}

TEST(AuditorAcceptsAnUnchangedSecondSnapshot) {
    auto d = scenario();
    const audit::Snapshot a = snap(*d);
    const audit::Snapshot b = snap(*d);
    const auto v = audit::check(b, &a);
    CHECK_MSG(v.empty(), dump(v));
}

TEST(AuditorAcceptsEveryStepOfAGrowingHistory) {
    Duel d("growing");
    audit::Auditor auditor;
    for (int i = 1; i <= 30; ++i) {
        d.submit(static_cast<OrderId>(i), i % 2 ? B : S, 100 + (i % 3), 1 + i % 5);
        if (i % 4 == 0) d.cancel(static_cast<OrderId>(i - 1));
        const auto v = auditor.audit(d.manager, d.ids);
        CHECK_MSG(v.empty(), dump(v));
    }
    CHECK(d.clean());
}

// ---- A: lifecycle <-> book ---------------------------------------------------------------

TEST(AuditorDetectsAnActiveOrderMissingFromTheBook) {
    auto d = scenario();
    audit::Snapshot s = snap(*d);
    // Drop the resting order 7 from its level (and keep the level's own totals consistent).
    for (auto& l : s.bids) {
        for (std::size_t i = 0; i < l.orders.size(); ++i) {
            if (l.orders[i].id == 7) {
                l.total -= l.orders[i].remaining;
                l.orders.erase(l.orders.begin() + static_cast<std::ptrdiff_t>(i));
                l.count = l.orders.size();
            }
        }
    }
    for (auto& l : s.asks) {
        for (std::size_t i = 0; i < l.orders.size(); ++i) {
            if (l.orders[i].id == 7) {
                l.total -= l.orders[i].remaining;
                l.orders.erase(l.orders.begin() + static_cast<std::ptrdiff_t>(i));
                l.count = l.orders.size();
            }
        }
    }
    s.bookOrders -= 1;
    CHECK(hasCode(audit::check(s), "A1"));
}

TEST(AuditorDetectsARestingOrderWithoutAnActiveRecord) {
    auto d = scenario();
    audit::Snapshot s = snap(*d);
    audit::RestingOrder ghost = s.asks.front().orders.front();
    ghost.id = 4242;
    s.asks.front().orders.push_back(ghost);
    s.asks.front().total += ghost.remaining;
    s.asks.front().count += 1;
    s.bookOrders += 1;
    CHECK(hasCode(audit::check(s), "A2"));
}

TEST(AuditorDetectsATerminalOrderStillInTheBook) {
    auto d = scenario();
    audit::Snapshot s = snap(*d);
    audit::RestingOrder zombie = s.asks.front().orders.front();
    zombie.id = 1;  // order 1 is FILLED
    s.asks.front().orders.push_back(zombie);
    s.asks.front().total += zombie.remaining;
    s.asks.front().count += 1;
    s.bookOrders += 1;
    const auto v = audit::check(s);
    CHECK(hasCode(v, "A3"));
}

TEST(AuditorDetectsTheSameIdRestingTwice) {
    auto d = scenario();
    audit::Snapshot s = snap(*d);
    audit::RestingOrder copy = s.bids.front().orders.front();
    s.asks.front().orders.push_back(copy);
    s.asks.front().total += copy.remaining;
    s.asks.front().count += 1;
    s.bookOrders += 1;
    CHECK(hasCode(audit::check(s), "E4"));
}

TEST(AuditorDetectsAnUntrackedOrForgottenRecord) {
    auto d = scenario();
    audit::Snapshot s = snap(*d);
    s.records[0].id = 0;  // "no such record"
    CHECK(hasCode(audit::check(s), "A5"));
    audit::Snapshot t = snap(*d);
    t.recorded += 1;  // the manager knows an id the caller did not track
    CHECK(hasCode(audit::check(t), "A5"));
}

TEST(AuditorDetectsInconsistentRejectedRecords) {
    auto d = scenario();
    audit::Snapshot s = snap(*d);
    lob::OrderRecord& r = s.records[indexOf(s, 9)];
    r.executedQuantity = 1;
    CHECK(hasCode(audit::check(s), "A7"));
}

// ---- B: quantities --------------------------------------------------------------------------------

TEST(AuditorDetectsQuantityReconciliationErrors) {
    auto d = scenario();
    audit::Snapshot s = snap(*d);
    s.records[indexOf(s, 2)].remainingQuantity += 1;
    CHECK(hasCode(audit::check(s), "B1"));

    audit::Snapshot n = snap(*d);
    n.records[indexOf(n, 2)].cancelledQuantity = -1;
    CHECK(hasCode(audit::check(n), "B2"));
}

TEST(AuditorDetectsStatusQuantityMismatches) {
    auto d = scenario();
    audit::Snapshot a = snap(*d);
    a.records[indexOf(a, 1)].status = OrderStatus::PartiallyFilled;  // a filled order claiming to be partial
    CHECK(hasCode(audit::check(a), "B3"));

    audit::Snapshot b = snap(*d);
    lob::OrderRecord& cancelled = b.records[indexOf(b, 3)];
    cancelled.remainingQuantity = 1;  // a cancelled order with remaining quantity
    cancelled.effectiveQuantity += 1;
    CHECK(hasCode(audit::check(b), "B3"));

    audit::Snapshot c = snap(*d);
    c.records[indexOf(c, 2)].status = OrderStatus::Filled;  // filled but not fully executed
    CHECK(hasCode(audit::check(c), "B3"));
}

TEST(AuditorDetectsExecutedQuantityDecreasing) {
    auto d = scenario();
    const audit::Snapshot before = snap(*d);
    audit::Snapshot after = before;
    lob::OrderRecord& r = after.records[indexOf(after, 2)];
    r.executedQuantity -= 1;
    r.remainingQuantity += 1;
    CHECK(hasCode(audit::check(after, &before), "B4"));
}

// ---- C: trades ----------------------------------------------------------------------------------------

TEST(AuditorDetectsTradeAssociationErrors) {
    auto d = scenario();
    {   // a trade listed twice for one order
        audit::Snapshot s = snap(*d);
        auto& list = s.tradesFor[indexOf(s, 6)];
        list.push_back(list.front());
        CHECK(hasCode(audit::check(s), "C2"));
    }
    {   // a trade missing from an order's history
        audit::Snapshot s = snap(*d);
        s.tradesFor[indexOf(s, 6)].pop_back();
        const auto v = audit::check(s);
        CHECK(hasCode(v, "C2") || hasCode(v, "C3"));
    }
    {   // a trade attached to an order that was not part of it
        audit::Snapshot s = snap(*d);
        s.tradesFor[indexOf(s, 5)].push_back(s.trades.front());
        CHECK(hasCode(audit::check(s), "C2"));
    }
    {   // history out of execution order
        audit::Snapshot s = snap(*d);
        auto& list = s.tradesFor[indexOf(s, 6)];
        CHECK(list.size() >= 2);
        std::swap(list.front(), list.back());
        CHECK(hasCode(audit::check(s), "C5"));
    }
}

TEST(AuditorDetectsExecutedQuantityThatDoesNotMatchTrades) {
    auto d = scenario();
    audit::Snapshot s = snap(*d);
    lob::OrderRecord& r = s.records[indexOf(s, 6)];
    r.executedQuantity -= 1;
    r.cancelledQuantity += 1;  // keep B1 satisfied; only the trade sum is now wrong
    CHECK(hasCode(audit::check(s), "C3"));
}

TEST(AuditorDetectsGlobalTradeListProblems) {
    auto d = scenario();
    audit::Snapshot dup = snap(*d);
    CHECK(dup.trades.size() >= 2);
    dup.trades[1] = dup.trades[0];  // the same trade id twice
    CHECK(hasCode(audit::check(dup), "C4"));
}

// ---- E: book integrity ----------------------------------------------------------------------------------------

TEST(AuditorDetectsBookOrderingAndAggregateErrors) {
    auto d = scenario();
    {
        audit::Snapshot s = snap(*d);
        CHECK(s.bids.size() >= 2);
        std::swap(s.bids.front(), s.bids.back());
        CHECK(hasCode(audit::check(s), "E1"));
    }
    {
        audit::Snapshot s = snap(*d);
        s.asks.front().total += 1;
        CHECK(hasCode(audit::check(s), "E3"));
    }
    {
        audit::Snapshot s = snap(*d);
        s.asks.front().orders.clear();
        s.asks.front().count = 0;
        s.asks.front().total = 0;
        CHECK(hasCode(audit::check(s), "E2"));
    }
    {
        audit::Snapshot s = snap(*d);
        s.bids.front().orders.front().side = Side::Sell;
        CHECK(hasCode(audit::check(s), "E2"));
    }
    {
        audit::Snapshot s = snap(*d);
        s.bids.front().orders.front().price += 1;
        CHECK(hasCode(audit::check(s), "E2"));
    }
}

TEST(AuditorDetectsBrokenFifoWithinALevel) {
    Duel d("fifo");
    d.submit(1, B, 100, 5);
    d.submit(2, B, 100, 5);
    d.submit(3, B, 100, 5);
    audit::Snapshot s = snap(d);
    CHECK(audit::check(s).empty());
    auto& orders = s.bids.front().orders;
    std::swap(orders.front(), orders.back());  // queue order no longer follows arrival sequence
    CHECK(hasCode(audit::check(s), "E2"));
}

TEST(AuditorDetectsACrossedBook) {
    auto d = scenario();
    audit::Snapshot s = snap(*d);
    s.bids.front().price = s.asks.front().price + 1;
    for (auto& o : s.bids.front().orders) o.price = s.bids.front().price;
    CHECK(hasCode(audit::check(s), "D1"));
}

TEST(AuditorDetectsWrongMarketData) {
    auto d = scenario();
    {
        audit::Snapshot s = snap(*d);
        s.spread = *s.spread + 1;
        CHECK(hasCode(audit::check(s), "E6"));
    }
    {
        audit::Snapshot s = snap(*d);
        s.mid->ticks += 1;
        CHECK(hasCode(audit::check(s), "E6"));
    }
    {
        audit::Snapshot s = snap(*d);
        s.bestAsk->price += 1;
        CHECK(hasCode(audit::check(s), "E6"));
    }
    {
        audit::Snapshot s = snap(*d);
        s.depth2.bids.front().totalQuantity += 1;
        CHECK(hasCode(audit::check(s), "E6"));
    }
}

// ---- T: transitions between snapshots ------------------------------------------------------------------------------

TEST(AuditorDetectsIdReuseAndChangedTerminalOrders) {
    auto d = scenario();
    const audit::Snapshot before = snap(*d);
    {   // a recorded id re-created as a different order
        audit::Snapshot s = before;
        s.records[indexOf(s, 1)].side = Side::Buy;
        CHECK(hasCode(audit::check(s, &before), "A6"));
    }
    {   // a terminal order that changed afterwards
        audit::Snapshot s = before;
        s.records[indexOf(s, 3)].cancelledQuantity += 1;
        s.records[indexOf(s, 3)].effectiveQuantity += 1;
        CHECK(hasCode(audit::check(s, &before), "A6"));
    }
    {   // a terminal order coming back to life
        audit::Snapshot s = before;
        s.records[indexOf(s, 1)].status = OrderStatus::New;
        CHECK(hasCode(audit::check(s, &before), "A6"));
    }
}

TEST(AuditorDetectsIllegalTransitionsAndLostHistory) {
    auto d = scenario();
    const audit::Snapshot before = snap(*d);
    {
        audit::Snapshot s = before;
        s.records[indexOf(s, 2)].status = OrderStatus::New;  // PARTIALLY_FILLED -> NEW
        CHECK(hasCode(audit::check(s, &before), "T3"));
    }
    {
        audit::Snapshot s = before;
        s.trades.pop_back();
        CHECK(hasCode(audit::check(s, &before), "T1"));
    }
    {
        audit::Snapshot s = before;
        s.recorded -= 1;
        CHECK(hasCode(audit::check(s, &before), "T2"));
    }
    {
        audit::Snapshot s = before;
        s.tradesFor[indexOf(s, 6)].pop_back();
        CHECK(hasCode(audit::check(s, &before), "T4"));
    }
}

// ---- F: stale history -------------------------------------------------------------------------------------------------------

TEST(AuditorIsAwareOfStaleHistory) {
    // Order 1 (SELL 10) has traded 4 with order 2 (BUY 4); book holds order 1 with 6 left.
    Duel d("stale");
    d.submit(1, S, 100, 10);
    d.submit(2, B, 100, 4);
    const audit::Snapshot healthy = snap(d);
    CHECK(audit::check(healthy).empty());

    // Simulate "the engine traded but the records have not caught up yet": un-apply the trade.
    audit::Snapshot stale = healthy;
    stale.current = false;
    for (std::size_t i = 0; i < stale.records.size(); ++i) {
        lob::OrderRecord& r = stale.records[i];
        r.executedQuantity = 0;
        r.remainingQuantity = r.effectiveQuantity;
        r.status = OrderStatus::New;
        r.tradeIndices.clear();
        stale.tradesFor[i].clear();
    }
    // As a stale snapshot this is legitimate (records lag; book and global trades are ahead).
    CHECK_MSG(audit::check(stale).empty(), dump(audit::check(stale)));
    // The very same data presented as "current" must be rejected.
    stale.current = true;
    const auto v = audit::check(stale);
    CHECK(!v.empty());
    CHECK(hasCode(v, "A1") || hasCode(v, "C2") || hasCode(v, "C3"));
}

TEST(AuditorStillCatchesSelfInconsistencyWhileStale) {
    Duel d("stale2");
    d.submit(1, S, 100, 10);
    d.submit(2, B, 100, 4);
    audit::Snapshot s = snap(d);
    s.current = false;
    s.records[0].remainingQuantity += 1;  // not explainable by lag: breaks the record's own arithmetic
    CHECK(hasCode(audit::check(s), "B1"));
}

TEST(AuditorCrossChecksProductionConsistencyAndTheStaleFlag) {
    // Healthy: production's checkConsistency agrees.
    Duel d("cross");
    d.submit(1, S, 100, 10);
    d.submit(2, B, 100, 4);
    audit::Auditor a;
    CHECK(a.audit(d.manager, d.ids).empty());
    CHECK(d.manager.isHistoryCurrent());
    CHECK(d.manager.checkConsistency().ok());
}

TEST_MAIN()
