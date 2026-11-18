// Boundary and edge-case tests, mostly run as production-vs-model duels with the
// system auditor, so every edge is checked against an independent expectation and
// against the global invariants, not just against a hand-written assertion.
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "Cli.h"
#include "CommandParser.h"
#include "Duel.h"
#include "MatchingEngine.h"
#include "PriceLevel.h"
#include "TestFramework.h"
#include "TestRng.h"

using namespace lob;

namespace {

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;
constexpr Price kMaxPrice = std::numeric_limits<Price>::max();
constexpr Quantity kMaxQty = std::numeric_limits<Quantity>::max();

#define DUEL_OK(d) CHECK_MSG((d).clean(), (d).failure())

}  // namespace

// ===== A. Numeric boundaries ===========================================================

TEST(MinimumPriceAndQuantityTrade) {
    Duel d("min");
    d.submit(1, S, 1, 1);
    d.submit(2, B, 1, 1);
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.trades().size(), 1u);
    CHECK_EQ(d.manager.trades()[0].price(), 1);
    CHECK_EQ(d.manager.trades()[0].quantity(), 1);
}

TEST(MaximumPriceRestsTradesAndHasExactMarketData) {
    Duel d("max-price");
    d.submit(1, B, kMaxPrice - 1, 5);
    d.submit(2, S, kMaxPrice, 5);
    // bid + ask would overflow int64: spread and mid must still be exact.
    CHECK_EQ(*d.manager.book().spread(), 1);
    CHECK_EQ(d.manager.book().midPrice()->ticks, kMaxPrice - 1);
    CHECK(d.manager.book().midPrice()->halfTick);
    d.submit(3, B, kMaxPrice, 5);    // trades with order 2 at INT64_MAX
    d.submit(4, S, kMaxPrice - 1, 5);  // trades with order 1 at INT64_MAX - 1
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.trades()[0].price(), kMaxPrice);
    CHECK_EQ(d.manager.trades()[1].price(), kMaxPrice - 1);
}

TEST(MidPriceBetweenTwoMaximalPricesDoesNotOverflow) {
    Duel d("mid-extremes");
    d.submit(1, B, kMaxPrice - 2, 1);
    d.submit(2, S, kMaxPrice, 1);
    CHECK_EQ(d.manager.book().midPrice()->ticks, kMaxPrice - 1);  // exact, no half tick
    CHECK(!d.manager.book().midPrice()->halfTick);
    d.checkEverything();
    DUEL_OK(d);
}

TEST(MaximumQuantityFillsExactly) {
    Duel d("max-qty");
    d.submit(1, S, 100, kMaxQty);
    d.submit(2, B, 100, kMaxQty);
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.order(1)->executedQuantity, kMaxQty);
    CHECK_EQ(d.manager.trades()[0].quantity(), kMaxQty);
    CHECK(d.manager.order(2)->status == OrderStatus::Filled);
}

TEST(MaximumQuantityPartialFills) {
    Duel d("max-qty-partial");
    d.submit(1, S, 100, kMaxQty);
    d.submit(2, B, 100, kMaxQty - 1);   // leaves exactly 1
    d.submit(3, B, 100, 1);
    d.checkEverything();
    DUEL_OK(d);
    CHECK(d.manager.order(1)->status == OrderStatus::Filled);
}

TEST(InvalidNumericInputsAreRejectedWithoutChangingTheBook) {
    Duel d("invalid-numerics");
    d.submit(1, B, 100, 10);
    const Price badPrices[] = {0, -1, -100, std::numeric_limits<Price>::min()};
    const Quantity badQuantities[] = {0, -1, -100, std::numeric_limits<Quantity>::min()};
    OrderId id = 10;
    for (Price p : badPrices) d.submit(id++, B, p, 5);
    for (Quantity q : badQuantities) d.submit(id++, S, 100, q);
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.book().orderCount(), 1u);          // only order 1 ever rested
    CHECK_EQ(d.manager.trades().size(), 0u);
    CHECK_EQ(d.manager.recordedOrderCount(), 9u);          // order 1 + 8 retired rejections
}

TEST(ModifyToInvalidOrExtremeValues) {
    Duel d("modify-numerics");
    d.submit(1, B, 100, 10);
    d.modify(1, 0, std::nullopt);
    d.modify(1, -5, std::nullopt);
    d.modify(1, std::nullopt, 0);
    d.modify(1, std::nullopt, -1);
    d.modify(1, std::nullopt, std::numeric_limits<Quantity>::min());
    d.modify(1, kMaxPrice, std::nullopt);       // legal: a huge price
    d.modify(1, std::nullopt, kMaxQty);         // legal: a huge quantity (no executions yet)
    d.checkEverything();
    DUEL_OK(d);
}

TEST(ModifyQuantityThatWouldOverflowEffectiveTotalIsRejected) {
    Duel d("effective-overflow");
    d.submit(1, B, 100, 100);
    d.submit(2, S, 100, 40);              // order 1 has executed 40
    d.modify(1, std::nullopt, kMaxQty);   // 40 + INT64_MAX would overflow
    d.modify(1, std::nullopt, kMaxQty - 39);  // 40 + (MAX - 39) = MAX + 1: still overflows
    d.modify(1, std::nullopt, kMaxQty - 40);  // exactly MAX: allowed
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.order(1)->effectiveQuantity, kMaxQty);
}

// ---- Defect regression: a price level's aggregate quantity must never overflow ----------

TEST(LevelAggregateOverflowIsRejectedNotWrapped) {
    Duel d("level-overflow");
    d.submit(1, S, 100, kMaxQty);
    const ExecutionReport r = d.submit(2, S, 100, 1);   // would make the level total INT64_MAX + 1
    d.checkEverything();
    DUEL_OK(d);
    CHECK(!r.accepted);
    CHECK(r.rejectReason == RejectReason::QuantityOverflow);
    CHECK(r.status == OrderStatus::Rejected);
    CHECK_EQ(d.manager.book().bestAsk()->totalQuantity, kMaxQty);   // not wrapped negative
    CHECK_EQ(d.manager.book().orderCount(), 1u);
    CHECK(d.manager.order(2)->status == OrderStatus::Rejected);
    CHECK(d.manager.submit(2, S, 100, 1).rejectReason == RejectReason::DuplicateOrderId);  // id retired
}

TEST(LevelAggregateAtExactlyMaxIsAllowed) {
    Duel d("level-exact");
    d.submit(1, S, 100, kMaxQty / 2 + 1);
    d.submit(2, S, 100, kMaxQty / 2);       // total = INT64_MAX exactly
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.book().bestAsk()->totalQuantity, kMaxQty);
    CHECK_EQ(d.manager.book().bestAsk()->orderCount, 2u);
}

TEST(LevelOverflowOnlyAffectsTheSameSideAndPrice) {
    Duel d("level-scope");
    d.submit(1, S, 100, kMaxQty);
    d.submit(2, S, 101, kMaxQty);            // different price: fine
    d.submit(3, B, 99, kMaxQty);             // other side: fine
    d.submit(4, S, 100, 1);                  // same level: rejected
    d.checkEverything();
    DUEL_OK(d);
    CHECK(d.manager.order(2)->status == OrderStatus::New);
    CHECK(d.manager.order(3)->status == OrderStatus::New);
    CHECK(d.manager.order(4)->status == OrderStatus::Rejected);
}

TEST(LevelCapacityIsReleasedWhenOrdersLeave) {
    Duel d("level-release");
    d.submit(1, S, 100, kMaxQty);
    d.submit(2, S, 100, 1);                  // rejected
    d.cancel(1);
    d.submit(3, S, 100, 5);                  // the level is empty again: accepted
    d.submit(4, S, 100, kMaxQty - 5);        // total exactly MAX
    d.submit(5, S, 100, 1);                  // rejected
    d.submit(6, B, 100, kMaxQty - 5);        // consumes order 3 (5) then 4's quantity...
    d.checkEverything();
    DUEL_OK(d);
}

TEST(ModifyingIntoAnOverflowingLevelIsRejectedAndLeavesTheOrderAlone) {
    Duel d("modify-overflow");
    d.submit(1, B, 100, kMaxQty / 2 + 1);
    d.submit(2, B, 100, kMaxQty / 2);        // level 100 total = MAX
    d.submit(3, B, 99, 10);
    const ExecutionReport up = d.modify(2, std::nullopt, kMaxQty / 2 + 1);  // same level, +1: overflow
    const ExecutionReport move = d.modify(3, 100, std::nullopt);             // moving 10 into a full level
    d.checkEverything();
    DUEL_OK(d);
    CHECK(!up.accepted);
    CHECK(up.rejectReason == RejectReason::QuantityOverflow);
    CHECK(!move.accepted);
    CHECK(move.rejectReason == RejectReason::QuantityOverflow);
    CHECK_EQ(d.manager.book().find(2)->remainingQuantity(), kMaxQty / 2);  // unchanged
    CHECK_EQ(d.manager.book().find(3)->price(), 99);                        // unchanged, same place
}

TEST(ModifyReplacingAnOrderInItsOwnLevelDoesNotCountItselfTwice) {
    Duel d("modify-self");
    d.submit(1, B, 100, kMaxQty / 2 + 1);
    d.submit(2, B, 100, kMaxQty / 2);
    // Order 2 grows to the largest value its level can hold once its own old quantity is removed.
    d.modify(2, std::nullopt, 10);                 // shrink in place
    d.modify(2, std::nullopt, kMaxQty / 2);        // grow back: its own old 10 must not count twice
    d.checkEverything();
    DUEL_OK(d);
}

TEST(IncomingOrderTooLargeForItsOwnLevelIsRejectedBeforeAnyTrade) {
    // Documented conservative rule: the check assumes the whole quantity could rest, so an
    // order that WOULD have matched fully is also refused, and nothing has executed.
    Duel d("conservative");
    d.submit(1, B, 100, 1);                     // a tiny bid at 100
    d.submit(2, S, 100, 1);                     // trades with it; bid level is empty again
    d.submit(3, B, 100, kMaxQty);               // bid level 100 holds MAX
    d.submit(4, S, 100, 10);                    // sell 10 against that bid: fine, trades
    d.submit(5, B, 100, 11);                    // level now holds MAX - 10; +11 would overflow: rejected
    d.checkEverything();
    DUEL_OK(d);
    CHECK(d.manager.order(5)->status == OrderStatus::Rejected);
    CHECK_EQ(d.manager.trades().size(), 2u);
}

TEST(LevelAndBookRejectOverflowDirectly) {
    // The lower layers defend themselves too, independently of the engine's pre-check.
    PriceLevel level(100);
    auto mk = [](OrderId id, Quantity q) { return *Order::create(id, S, OrderType::Limit, 100, q, id); };
    level.add(mk(1, kMaxQty));
    CHECK_THROWS(level.add(mk(2, 1)), std::invalid_argument);
    CHECK_EQ(level.totalQuantity(), kMaxQty);
    CHECK_EQ(level.orderCount(), 1u);
    CHECK(level.checkInvariants());

    OrderBook book;
    CHECK(book.add(mk(1, kMaxQty)) == RejectReason::None);
    CHECK(book.add(mk(2, 1)) == RejectReason::QuantityOverflow);
    CHECK(!book.contains(2));
    CHECK_EQ(book.bestAsk()->totalQuantity, kMaxQty);
    CHECK(book.checkInvariants().ok());
}

TEST(BookRejectsLevelOverflowOnTheBidSideToo) {
    // (found by the coverage run: only the ask side had been exercised directly)
    OrderBook book;
    auto mk = [](OrderId id, Quantity q) { return *Order::create(id, B, OrderType::Limit, 100, q, id); };
    CHECK(book.add(mk(1, kMaxQty)) == RejectReason::None);
    CHECK(book.add(mk(2, 1)) == RejectReason::QuantityOverflow);
    CHECK_EQ(book.bestBid()->totalQuantity, kMaxQty);
    CHECK(book.checkInvariants().ok());
    book.cancel(1);
    CHECK(book.add(mk(2, 1)) == RejectReason::None);   // room again once the big order left
}

TEST(EngineReplaceRejectsBadRequestsItselfAndLeavesTheOrderResting) {
    // The OrderManager validates first, so these engine-level rejections are only reachable
    // by calling the engine directly.
    MatchingEngine e;
    auto mk = [](OrderId id, Side s, Price p, Quantity q, Sequence seq) {
        return *Order::create(id, s, OrderType::Limit, p, q, seq);
    };
    e.submit(mk(1, B, 100, 10, 1));
    e.submit(mk(2, S, 100, 4, 2));          // order 1 has executed 4, 6 remain
    CHECK(e.replace(99, 100, 5, 3).rejectReason == RejectReason::OrderNotFound);
    CHECK(e.replace(1, 0, 5, 3).rejectReason == RejectReason::InvalidPrice);
    CHECK(e.replace(1, -2, 5, 3).rejectReason == RejectReason::InvalidPrice);
    CHECK(e.replace(1, 100, 0, 3).rejectReason == RejectReason::InvalidQuantity);
    CHECK(e.replace(1, 100, -1, 3).rejectReason == RejectReason::InvalidQuantity);
    CHECK(e.replace(1, 100, kMaxQty, 3).rejectReason == RejectReason::InvalidQuantity);   // 4 + MAX overflows
    // Nothing above changed the order: still resting, 6 remaining, 4 executed.
    CHECK(e.book().contains(1));
    CHECK_EQ(e.book().find(1)->remainingQuantity(), 6);
    CHECK_EQ(e.book().find(1)->executedQuantity(), 4);
    CHECK(e.book().checkInvariants().ok());
    // And the engine-level overflow rule: moving into a level that is already full.
    e.submit(mk(3, B, 90, kMaxQty, 4));
    e.submit(mk(4, B, 80, 5, 5));
    CHECK(e.replace(4, 90, 5, 6).rejectReason == RejectReason::QuantityOverflow);
    CHECK_EQ(e.book().find(4)->price(), 80);
    CHECK(e.book().checkInvariants().ok());
}

TEST(TradeIdExhaustionForIncomingSellsAgainstBids) {
    // (found by the coverage run: only the incoming-BUY direction had been exercised)
    const TradeId maxTrade = std::numeric_limits<TradeId>::max();
    OrderManager m(1, maxTrade - 1);        // two trade ids left
    audit::Auditor auditor;
    std::vector<OrderId> ids = {1, 2, 3, 4, 5};
    for (OrderId i = 1; i <= 3; ++i) m.submit(i, B, 100, 10);
    const ExecutionReport refused = m.submit(4, S, 100, 30);     // needs three trades
    CHECK(!refused.accepted);
    CHECK(refused.rejectReason == RejectReason::TradeIdsExhausted);
    CHECK_EQ(m.trades().size(), 0u);
    CHECK_EQ(m.book().orderCount(), 3u);
    const ExecutionReport ok = m.submit(5, S, 100, 20);          // two trades: fits exactly
    CHECK(ok.accepted);
    CHECK_EQ(m.trades().back().id(), maxTrade);
    CHECK(m.submit(6, S, 100, 5).rejectReason == RejectReason::TradeIdsExhausted);
    ids.push_back(6);
    const auto v = auditor.audit(m, ids);
    for (const auto& s : v) std::cerr << "    " << s << "\n";
    CHECK(v.empty());
}

TEST(OverflowRejectionHasAReadableReason) {
    CHECK(!toString(RejectReason::QuantityOverflow).empty());
    CHECK(toString(RejectReason::QuantityOverflow).find("overflow") != std::string::npos);
}

// ---- Id and counter exhaustion -------------------------------------------------------

TEST(OrderIdAtTheTypeMaximumWorksAndGeneratedIdsNeverWrap) {
    const OrderId maxId = std::numeric_limits<OrderId>::max();
    Duel d("id-max", 0, 1, maxId - 1);
    d.submit(B, 100, 1);                  // generated: maxId - 1
    d.submit(B, 100, 1);                  // generated: maxId
    d.submit(B, 100, 1);                  // exhausted
    d.submit(B, 100, 1);                  // still exhausted
    d.submit(5, S, 100, 2);               // client ids still work
    d.cancel(maxId);
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.recordedOrderCount(), 3u);   // two generated + the client id; nothing reused
}

TEST(ClientIdAtTheTypeMaximum) {
    Duel d("client-max");
    const OrderId maxId = std::numeric_limits<OrderId>::max();
    d.submit(maxId, B, 100, 1);
    d.submit(maxId, S, 100, 1);           // duplicate: rejected
    d.modify(maxId, 101, std::nullopt);
    d.cancel(maxId);
    d.checkEverything();
    DUEL_OK(d);
}

TEST(TradeIdExhaustionIsRefusedAtomicallyAndAuditsClean) {
    const TradeId maxTrade = std::numeric_limits<TradeId>::max();
    OrderManager m(1, maxTrade - 1);       // exactly two trade ids left
    audit::Auditor auditor;
    std::vector<OrderId> ids;
    auto step = [&](OrderId id, const ExecutionReport& r) {
        ids.push_back(id);
        (void)r;
        const auto v = auditor.audit(m, ids);
        for (const auto& s : v) std::cerr << "    " << s << "\n";
        CHECK(v.empty());
    };
    step(1, m.submit(1, S, 100, 10));
    step(2, m.submit(2, S, 100, 10));
    step(3, m.submit(3, S, 100, 10));
    const ExecutionReport refused = m.submit(4, B, 100, 30);    // needs three trades
    step(4, refused);
    CHECK(!refused.accepted);
    CHECK(refused.rejectReason == RejectReason::TradeIdsExhausted);
    CHECK_EQ(m.trades().size(), 0u);                             // nothing executed
    CHECK_EQ(m.book().orderCount(), 3u);
    const ExecutionReport ok = m.submit(5, B, 100, 20);          // two trades: fits exactly
    step(5, ok);
    CHECK(ok.accepted);
    CHECK_EQ(m.trades().back().id(), maxTrade);
    const ExecutionReport none = m.submit(6, B, 100, 5);         // no ids left, would trade
    step(6, none);
    CHECK(!none.accepted);
    CHECK(m.submit(7, B, 90, 5).accepted);                       // not trading: still fine
}

// ===== B. Matching boundaries =====================================================================

TEST(IncomingBuyJustAboveJustBelowAndExactlyAtTheBestAsk) {
    Duel d("buy-edges");
    d.submit(1, S, 100, 10);
    d.submit(2, B, 99, 4);     // just below: rests, no trade
    d.submit(3, B, 100, 4);    // exactly at the ask: trades at 100
    d.submit(4, B, 101, 4);    // just above: trades at 100 (the resting price)
    d.submit(5, B, 101, 4);    // 2 left: trades 2, rests 2 at 101
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.trades().size(), 3u);
    for (const Trade& t : d.manager.trades()) CHECK_EQ(t.price(), 100);
}

TEST(IncomingSellJustBelowJustAboveAndExactlyAtTheBestBid) {
    Duel d("sell-edges");
    d.submit(1, B, 100, 10);
    d.submit(2, S, 101, 4);    // just above: rests
    d.submit(3, S, 100, 4);    // exactly at the bid: trades at 100
    d.submit(4, S, 99, 4);     // just below: trades at 100
    d.submit(5, S, 99, 4);     // 2 left: trades 2, rests 2 at 99
    d.checkEverything();
    DUEL_OK(d);
    for (const Trade& t : d.manager.trades()) CHECK_EQ(t.price(), 100);
}

TEST(OneToOneOneToManyAndManyToOne) {
    Duel d("fan");
    d.submit(1, S, 100, 10);
    d.submit(2, B, 100, 10);               // one to one
    for (OrderId i = 10; i < 20; ++i) d.submit(i, S, 100, 3);
    d.submit(30, B, 100, 30);              // one to many: ten makers
    d.submit(40, S, 100, 90);              // a big maker...
    for (OrderId i = 50; i < 60; ++i) d.submit(i, B, 100, 9);   // ...hit by many takers
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.tradesFor(30).size(), 10u);
    CHECK_EQ(d.manager.tradesFor(40).size(), 10u);
}

TEST(PartialRestingPartialIncomingExactAndNoLiquidity) {
    Duel d("partials");
    d.submit(1, S, 100, 10);
    d.submit(2, B, 100, 4);      // partial fill of the resting order
    d.submit(3, B, 100, 20);     // incoming larger than what is left: 6 trade, 14 rest
    d.submit(4, S, 100, 14);     // exact full fill of order 3's remainder
    d.submit(5, S, 100, 5);      // no liquidity: rests
    d.checkEverything();
    DUEL_OK(d);
    CHECK(d.manager.order(1)->status == OrderStatus::Filled);
    CHECK(d.manager.order(3)->status == OrderStatus::Filled);
    CHECK(d.manager.order(5)->status == OrderStatus::New);
}

TEST(SweepAcrossManyLevelsThenRestTheRemainder) {
    Duel d("sweep");
    for (OrderId i = 1; i <= 50; ++i) d.submit(i, S, 100 + static_cast<Price>(i), 2);
    d.submit(100, B, 125, 1000);            // takes 25 levels, rests 950 at 125
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.tradesFor(100).size(), 25u);
    CHECK_EQ(d.manager.book().bestBid()->price, 125);
    CHECK_EQ(d.manager.book().bestBid()->totalQuantity, 950);
}

// ===== C. Lifecycle boundaries ======================================================================

TEST(DuplicateActiveAndTerminalIds) {
    Duel d("dup-ids");
    d.submit(1, B, 100, 10);
    d.submit(1, B, 100, 10);       // duplicate active
    d.submit(2, S, 100, 10);       // fills 1
    d.submit(1, B, 100, 10);       // duplicate terminal (filled)
    d.cancel(2);                   // filled: cannot cancel
    d.submit(3, B, 90, 10);
    d.cancel(3);
    d.submit(3, B, 90, 10);        // duplicate terminal (cancelled)
    d.submit(4, B, 0, 10);         // rejected record
    d.submit(4, B, 90, 10);        // duplicate terminal (rejected)
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.recordedOrderCount(), 4u);
}

TEST(CancelAndModifyOfUnknownAndTerminalOrders) {
    Duel d("terminal-ops");
    d.submit(1, B, 100, 10);
    d.cancel(1);
    d.cancel(1);                       // already cancelled
    d.cancel(404);                     // unknown
    d.modify(404, 100, 5);             // unknown
    d.modify(1, 101, 5);               // terminal
    d.modify(1, std::nullopt, 0);      // terminal, and zero quantity
    d.submit(2, B, 100, 10);
    d.submit(3, S, 100, 10);
    d.cancel(2);                       // filled
    d.modify(2, 105, 5);               // filled
    d.checkEverything();
    DUEL_OK(d);
}

TEST(ModifyEveryWayOnPartiallyFilledOrders) {
    Duel d("modify-partial");
    d.submit(1, B, 100, 100);
    d.submit(2, S, 100, 30);                // order 1: 30 executed, 70 left
    d.modify(1, std::nullopt, 20);          // smaller
    d.modify(1, std::nullopt, 20);          // unchanged
    d.modify(1, std::nullopt, 0);           // zero
    d.modify(1, std::nullopt, 90);          // larger
    d.modify(1, 101, std::nullopt);         // price only
    d.modify(1, 99, 50);                    // price and quantity
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.order(1)->executedQuantity, 30);   // never altered
    CHECK_EQ(d.manager.order(1)->effectiveQuantity, 80);  // 30 executed + 50 remaining
}

TEST(PriorityPreservedByReductionAndLostByIncreaseOrPriceChange) {
    // Three bids at 100 in arrival order 1, 2, 3; each variant then lets one big SELL show
    // who is first in the queue.
    auto firstMaker = [](auto&& action) {
        Duel d("priority");
        d.submit(1, B, 100, 10);
        d.submit(2, B, 100, 10);
        d.submit(3, B, 100, 10);
        action(d);
        d.submit(9, S, 100, 1);
        d.checkEverything();
        CHECK_MSG(d.clean(), d.failure());
        return d.manager.trades().at(0).makerOrderId();
    };
    CHECK_EQ(firstMaker([](Duel&) {}), 1u);                                           // baseline
    CHECK_EQ(firstMaker([](Duel& d) { d.modify(1, std::nullopt, 4); }), 1u);          // reduction: keeps priority
    CHECK_EQ(firstMaker([](Duel& d) { d.modify(1, std::nullopt, 40); }), 2u);         // increase: loses it
    CHECK_EQ(firstMaker([](Duel& d) { d.modify(1, 101, std::nullopt); }), 1u);        // moved to the better price 101
    CHECK_EQ(firstMaker([](Duel& d) { d.modify(1, 99, std::nullopt); }), 2u);         // worse price: leaves the level
    CHECK_EQ(firstMaker([](Duel& d) { d.modify(3, std::nullopt, 4); }), 1u);          // reducing a later order changes nothing
}

TEST(NoChangeModificationKeepsEverything) {
    Duel d("nochange");
    d.submit(1, B, 100, 10);
    d.submit(2, B, 100, 10);
    const Sequence before = d.manager.order(1)->sequence;
    d.modify(1, 100, 10);
    d.modify(1, std::nullopt, 10);
    d.modify(1, 100, std::nullopt);
    d.modify(1, std::nullopt, std::nullopt);
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.order(1)->sequence, before);
}

// ===== D. Order book boundaries ======================================================================

TEST(EmptyBookAndSingleOrders) {
    Duel d("empty-and-single");
    d.checkEverything();                      // empty book audits clean
    DUEL_OK(d);
    CHECK(d.manager.book().empty());
    CHECK(!d.manager.book().bestBid().has_value());
    d.submit(1, B, 100, 5);
    d.checkEverything();
    CHECK(!d.manager.book().bestAsk().has_value());
    d.cancel(1);
    d.submit(2, S, 100, 5);
    d.checkEverything();
    DUEL_OK(d);
    CHECK(!d.manager.book().bestBid().has_value());
}

TEST(RemovalOfTheOnlyOrderBestBidAndBestAsk) {
    Duel d("removals");
    d.submit(1, B, 100, 5);
    d.submit(2, B, 99, 5);
    d.submit(3, S, 105, 5);
    d.submit(4, S, 106, 5);
    d.cancel(1);                            // best bid (also the only order at 100)
    CHECK_EQ(d.manager.book().bestBid()->price, 99);
    d.cancel(3);                            // best ask
    CHECK_EQ(d.manager.book().bestAsk()->price, 106);
    d.cancel(2);
    d.cancel(4);
    d.checkEverything();
    DUEL_OK(d);
    CHECK(d.manager.book().empty());
    CHECK_EQ(d.manager.book().bidLevelCount(), 0u);
}

TEST(CancellationOfOldestMiddleAndNewestInAQueue) {
    Duel d("queue-cancels");
    for (OrderId i = 1; i <= 9; ++i) d.submit(i, B, 100, 3);
    d.cancel(1);   // oldest
    d.cancel(5);   // middle
    d.cancel(9);   // newest
    d.submit(10, S, 100, 100);   // the survivors fill in arrival order: 2 3 4 6 7 8
    d.checkEverything();
    DUEL_OK(d);
    const auto& trades = d.manager.trades();
    CHECK_EQ(trades.size(), 6u);
    const OrderId expected[] = {2, 3, 4, 6, 7, 8};
    for (std::size_t i = 0; i < 6; ++i) CHECK_EQ(trades[i].makerOrderId(), expected[i]);
}

TEST(RepeatedAddRemoveCyclesAtOneLevelAndAcrossLevels) {
    Duel d("cycles", 0, 25);
    for (int round = 0; round < 300; ++round) {
        const OrderId id = static_cast<OrderId>(round + 1);
        d.submit(id, B, 100 + (round % 3), 5);
        if (round % 2 == 1) d.cancel(id - 1);
        if (round % 7 == 6) d.cancel(id);
    }
    d.checkEverything();
    DUEL_OK(d);
}

TEST(ThousandsOfPriceLevels) {
    Duel d("many-levels", 0, 500);
    for (OrderId i = 1; i <= 2000; ++i) d.submit(i, B, static_cast<Price>(i), 1);
    for (OrderId i = 2001; i <= 4000; ++i) d.submit(i, S, static_cast<Price>(i), 1);
    CHECK_EQ(d.manager.book().bidLevelCount(), 2000u);
    CHECK_EQ(d.manager.book().askLevelCount(), 2000u);
    d.submit(5000, B, 4000, 3000);           // sweeps 2000 ask levels, rests the remainder
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.book().askLevelCount(), 0u);
}

// ===== E. Parser and CLI boundaries ======================================================================

namespace {

// Independent validator for "is this token a valid plain-decimal int64 >= minimum?",
// written without std::from_chars and without any shared code with the parser.
bool expectedValidWholeNumber(const std::string& t, bool wantPositive, bool& outOfRange) {
    outOfRange = false;
    std::size_t i = 0;
    bool negative = false;
    if (i < t.size() && t[i] == '-') {
        negative = true;
        ++i;
    }
    if (i >= t.size()) return false;
    for (std::size_t k = i; k < t.size(); ++k) {
        if (t[k] < '0' || t[k] > '9') return false;
    }
    std::string digits = t.substr(i);
    const std::size_t nz = digits.find_first_not_of('0');
    digits = nz == std::string::npos ? "0" : digits.substr(nz);
    const std::string limit = negative ? "9223372036854775808" : "9223372036854775807";
    if (digits.size() > limit.size() || (digits.size() == limit.size() && digits > limit)) {
        outOfRange = true;
        return false;
    }
    if (digits == "0") return !wantPositive;
    return !negative || !wantPositive;
}

std::string randomToken(TestRng& rng) {
    static const char kAlphabet[] = "0123456789-+.eExXabc, ";
    const std::uint64_t style = rng.below(6);
    std::string t;
    if (style == 0) {                         // pure digits of random length, sometimes near the limit
        const std::size_t len = 1 + rng.below(21);
        for (std::size_t i = 0; i < len; ++i) t += static_cast<char>('0' + rng.below(10));
    } else if (style == 1) {                  // near INT64_MAX / MIN boundaries
        static const char* const edge[] = {"9223372036854775806", "9223372036854775807", "9223372036854775808",
                                           "9223372036854775809", "-9223372036854775807", "-9223372036854775808",
                                           "-9223372036854775809", "0", "-0", "00", "1", "-1", "18446744073709551615"};
        t = edge[rng.below(13)];
    } else if (style == 2) {                  // digits with one stray character
        const std::size_t len = 1 + rng.below(8);
        for (std::size_t i = 0; i < len; ++i) t += static_cast<char>('0' + rng.below(10));
        t.insert(rng.below(t.size() + 1), 1, kAlphabet[10 + rng.below(11)]);
    } else {                                  // arbitrary short strings from the alphabet
        const std::size_t len = 1 + rng.below(6);
        for (std::size_t i = 0; i < len; ++i) {
            const char c = kAlphabet[rng.below(sizeof(kAlphabet) - 2)];  // no space inside a token
            t += c;
        }
    }
    return t;
}

}  // namespace

TEST(ParserNumericStrictnessMatchesAnIndependentValidator) {
    TestRng rng(20260314);
    std::size_t accepted = 0, rejectedFormat = 0, rejectedRange = 0;
    const int failuresBefore = testfw::failureCount();
    for (int i = 0; i < 30000; ++i) {
        const std::string token = randomToken(rng);
        bool outOfRange = false;
        const bool wantOk = expectedValidWholeNumber(token, /*wantPositive=*/true, outOfRange);
        const ParseResult r = parseCommand("submit BUY " + token + " 5");
        CHECK_MSG(r.ok() == wantOk, "price token [" + token + "]: expected " + (wantOk ? "accept" : "reject") + ", parser said " +
                                        (r.ok() ? "accept" : "reject: " + r.error));
        if (r.ok()) {
            CHECK_MSG(std::to_string(r.command.price).size() <= token.size() + 1, "accepted value looks wrong for [" + token + "]");
            ++accepted;
        } else if (outOfRange) {
            CHECK_MSG(r.error.find("out of range") != std::string::npos || r.error.find("positive") != std::string::npos,
                      "out-of-range token [" + token + "] gave: " + r.error);
            ++rejectedRange;
        } else {
            ++rejectedFormat;
        }
        if (testfw::failureCount() != failuresBefore) break;  // one reproducible failure is enough
    }
    CHECK(accepted > 500);          // the generator really produces valid numbers,
    CHECK(rejectedFormat > 5000);   // malformed ones,
    CHECK(rejectedRange > 500);     // and out-of-range ones
}

TEST(AcceptedNumbersRoundTripExactly) {
    TestRng rng(99);
    for (int i = 0; i < 5000; ++i) {
        const Price p = rng.between(1, 1000000000000LL) * (rng.chance(10) ? 9000000 : 1);
        const Quantity q = rng.between(1, 1000000000);
        const ParseResult r = parseCommand("submit SELL " + std::to_string(p) + " " + std::to_string(q));
        CHECK(r.ok());
        CHECK_EQ(r.command.price, p);
        CHECK_EQ(r.command.quantity, q);
    }
}

TEST(ParserAndCliSurviveRandomByteGarbage) {
    OrderManager manager;
    std::ostringstream out, err;
    Cli cli(manager, out, err, Cli::Options{false});
    TestRng rng(424242);
    std::size_t parsedOk = 0;
    for (int i = 0; i < 6000; ++i) {
        std::string line;
        const std::size_t len = rng.below(80);
        for (std::size_t k = 0; k < len; ++k) line += static_cast<char>(rng.below(256));   // any byte, incl. NUL
        const std::size_t recordsBefore = manager.recordedOrderCount();
        const ParseResult r = parseCommand(line);          // must never throw
        out.str("");
        err.str("");
        cli.handleLine(line);                               // must never throw either
        if (r.ok()) ++parsedOk;
        else CHECK_MSG(manager.recordedOrderCount() == recordsBefore, "an unparseable line changed the system");
    }
    CHECK(manager.checkConsistency().ok());
    CHECK(manager.book().checkInvariants().ok());
    (void)parsedOk;
}

TEST(TabsMixedCaseAndRepeatedWhitespaceAcrossAllCommands) {
    OrderManager m;
    std::ostringstream out, err;
    Cli cli(m, out, err, Cli::Options{false});
    for (const char* line : {"\tSuBmIt\t\tBuY   100\t5", " SUBMIT   sell 100   5 ", "\t\tBOOK\t", "  Best  ", "DePtH\t2",
                             "  order   1 ", "\tTRADES 1\t", "  CaNcEl 99  ", "moDIFY  3   100\t4", "\t help\t ", "  RECOVER "}) {
        out.str("");
        err.str("");
        cli.handleLine(line);
        CHECK_MSG(err.str().empty() || std::string(line).find("moDIFY") != std::string::npos, std::string("[") + line + "] -> " + err.str());
    }
    CHECK(m.trades().size() == 1u);       // the buy and the sell above crossed
    CHECK(m.checkConsistency().ok());
}

TEST(RepeatedIdenticalCommandsAreEachExecutedOnce) {
    OrderManager m;
    std::ostringstream out, err;
    Cli cli(m, out, err, Cli::Options{false});
    for (int i = 0; i < 100; ++i) cli.handleLine("submit BUY 100 1");
    CHECK_EQ(m.recordedOrderCount(), 100u);            // one record per command, no merging
    CHECK_EQ(m.book().bestBid()->orderCount, 100u);
    for (int i = 0; i < 100; ++i) cli.handleLine("cancel 7");   // the same cancel again and again
    CHECK(m.order(7)->status == OrderStatus::Cancelled);
    CHECK_EQ(m.book().bestBid()->orderCount, 99u);
    CHECK(m.checkConsistency().ok());
}

TEST(EndOfInputInTheMiddleOfNormalUse) {
    OrderManager m;
    std::ostringstream out, err;
    Cli cli(m, out, err, Cli::Options{false});
    std::istringstream in("submit BUY 100 5\nsubmit SELL 100 2\nord");   // EOF inside a half-typed command
    CHECK_EQ(cli.run(in), 0);
    CHECK_EQ(m.trades().size(), 1u);
    CHECK(!err.str().empty());   // the half-typed command is reported, not executed
    CHECK(m.checkConsistency().ok());
}

TEST_MAIN()
