#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "MatchingEngine.h"
#include "TestFramework.h"

using namespace lob;

namespace {

Sequence g_seq = 0;  // strictly increasing arrival counter

Order mk(OrderId id, Side side, Price price, Quantity qty) {
    auto o = Order::create(id, side, OrderType::Limit, price, qty, ++g_seq);
    if (!o) throw std::runtime_error("test setup: order creation failed");
    return *o;
}
Order buy(OrderId id, Price p, Quantity q) { return mk(id, Side::Buy, p, q); }
Order sell(OrderId id, Price p, Quantity q) { return mk(id, Side::Sell, p, q); }

// One execution reduced to the facts the specification pins down.
struct Exec {
    OrderId maker;
    OrderId taker;
    Price price;
    Quantity qty;
    bool operator==(const Exec& o) const {
        return maker == o.maker && taker == o.taker && price == o.price && qty == o.qty;
    }
};

std::vector<Exec> execs(const std::vector<Trade>& trades) {
    std::vector<Exec> out;
    for (const Trade& t : trades) {
        out.push_back({t.makerOrderId(), t.takerOrderId(), t.price(), t.quantity()});
    }
    return out;
}
std::vector<Exec> execs(const ExecutionReport& r) { return execs(r.trades); }

bool consistent(const OrderBook& book) {
    const InvariantReport r = book.checkInvariants();
    for (const auto& v : r.violations) std::cerr << "    invariant violated: " << v << "\n";
    return r.ok();
}

bool notCrossed(const OrderBook& b) {
    auto bid = b.bestBid();
    auto ask = b.bestAsk();
    return !bid || !ask || bid->price < ask->price;
}

// Rests orders directly (no opposing liquidity), keeping the helpers honest.
void rest(MatchingEngine& e, Order o) {
    ExecutionReport r = e.submit(std::move(o));
    if (!r.accepted || r.tradeCount() != 0) throw std::runtime_error("test setup: order traded");
}

std::vector<OrderId> idsAt(const PriceLevel& level) {
    std::vector<OrderId> out;
    for (const Order& o : level) out.push_back(o.id());
    return out;
}

}  // namespace

// ---- Mandatory scenarios ------------------------------------------------

TEST(Scenario1_BasicMatch) {
    MatchingEngine e;
    rest(e, sell(1, 105, 100));
    ExecutionReport r = e.submit(buy(2, 105, 100));
    CHECK(r.accepted);
    CHECK_EQ(r.tradeCount(), 1u);
    CHECK((execs(r) == std::vector<Exec>{{1, 2, 105, 100}}));
    CHECK(r.status == OrderStatus::Filled);
    CHECK_EQ(r.executedQuantity, 100);
    CHECK_EQ(r.remainingQuantity, 0);
    CHECK(!r.rested());
    CHECK(e.book().empty());
    CHECK_EQ(e.book().bidLevelCount(), 0u);
    CHECK_EQ(e.book().askLevelCount(), 0u);
    CHECK(consistent(e.book()));
}

TEST(Scenario2_PriceImprovementExecutesAtRestingPrice) {
    MatchingEngine e;
    rest(e, sell(1, 105, 100));
    ExecutionReport r = e.submit(buy(2, 110, 100));
    CHECK_EQ(r.tradeCount(), 1u);
    CHECK_EQ(r.trades[0].price(), 105);  // not 110
    CHECK(e.book().empty());
}

TEST(Scenario2b_SellSideExecutesAtRestingBidPrice) {
    MatchingEngine e;
    rest(e, buy(1, 100, 100));
    ExecutionReport r = e.submit(sell(2, 95, 100));
    CHECK((execs(r) == std::vector<Exec>{{1, 2, 100, 100}}));  // not 95
    CHECK(e.book().empty());
}

TEST(Scenario3_PartialRestingFill) {
    MatchingEngine e;
    rest(e, sell(1, 105, 100));
    rest(e, sell(3, 105, 7));  // behind order 1
    ExecutionReport r = e.submit(buy(2, 105, 40));
    CHECK_EQ(r.tradeCount(), 1u);
    CHECK_EQ(r.trades[0].quantity(), 40);
    CHECK(r.status == OrderStatus::Filled);
    const Order* maker = e.book().find(1);
    CHECK(maker != nullptr);
    if (maker != nullptr) {
        CHECK_EQ(maker->remainingQuantity(), 60);
        CHECK(maker->status() == OrderStatus::PartiallyFilled);
    }
    CHECK(e.book().contains(1));  // lookup entry survives a partial fill
    // The partially filled maker keeps its place at the front of the queue.
    CHECK((idsAt(e.book().asks().at(105)) == std::vector<OrderId>{1, 3}));
    CHECK_EQ(e.book().bestAsk()->totalQuantity, 67);  // aggregate updated
    CHECK(!e.book().contains(2));
    CHECK(consistent(e.book()));
}

TEST(Scenario4_PartialIncomingFillRestsRemainder) {
    MatchingEngine e;
    rest(e, sell(1, 105, 40));
    ExecutionReport r = e.submit(buy(2, 105, 100));
    CHECK_EQ(r.tradeCount(), 1u);
    CHECK_EQ(r.trades[0].quantity(), 40);
    CHECK(r.accepted);
    CHECK(r.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(r.executedQuantity, 40);
    CHECK_EQ(r.remainingQuantity, 60);
    CHECK(r.rested());
    CHECK(!e.book().contains(1));  // maker fully consumed
    CHECK_EQ(e.book().askLevelCount(), 0u);
    CHECK_EQ(e.book().bidLevelCount(), 1u);
    const Order* rested = e.book().find(2);
    CHECK(rested != nullptr);
    if (rested != nullptr) {
        CHECK_EQ(rested->remainingQuantity(), 60);
        CHECK_EQ(rested->price(), 105);
        CHECK_EQ(rested->originalQuantity(), 100);
    }
    CHECK_EQ(e.book().bestBid()->price, 105);
    CHECK(consistent(e.book()));
}

TEST(Scenario5_FifoPriority) {
    MatchingEngine e;
    rest(e, sell(1, 105, 30));  // A
    rest(e, sell(2, 105, 40));  // B
    rest(e, sell(3, 105, 50));  // C
    ExecutionReport r = e.submit(buy(4, 105, 60));
    CHECK_EQ(r.tradeCount(), 2u);
    CHECK((execs(r) == std::vector<Exec>{{1, 4, 105, 30}, {2, 4, 105, 30}}));
    CHECK(r.status == OrderStatus::Filled);
    CHECK(!e.book().contains(1));                       // A filled completely
    CHECK_EQ(e.book().find(2)->remainingQuantity(), 10);  // B partial
    CHECK_EQ(e.book().find(3)->remainingQuantity(), 50);  // C untouched
    CHECK(e.book().find(3)->status() == OrderStatus::New);
    CHECK((idsAt(e.book().asks().at(105)) == std::vector<OrderId>{2, 3}));
    CHECK_EQ(e.book().bestAsk()->totalQuantity, 60);
    CHECK(consistent(e.book()));
}

TEST(Scenario6_MultiplePriceLevelsStopAtLimit) {
    MatchingEngine e;
    rest(e, sell(1, 103, 30));
    rest(e, sell(2, 105, 40));
    rest(e, sell(3, 108, 50));
    ExecutionReport r = e.submit(buy(4, 106, 100));
    CHECK((execs(r) == std::vector<Exec>{{1, 4, 103, 30}, {2, 4, 105, 40}}));
    CHECK(r.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(r.remainingQuantity, 30);
    CHECK(e.book().contains(3));  // 108 is beyond the limit
    CHECK_EQ(e.book().find(3)->remainingQuantity(), 50);
    CHECK_EQ(e.book().bestBid()->price, 106);
    CHECK_EQ(e.book().bestBid()->totalQuantity, 30);
    CHECK_EQ(e.book().bestAsk()->price, 108);
    CHECK(notCrossed(e.book()));
    CHECK(consistent(e.book()));
}

TEST(Scenario7_IncomingSellMatchesBids) {
    MatchingEngine e;
    rest(e, buy(1, 105, 30));  // A
    rest(e, buy(2, 103, 40));  // B
    ExecutionReport r = e.submit(sell(3, 103, 50));
    CHECK((execs(r) == std::vector<Exec>{{1, 3, 105, 30}, {2, 3, 103, 20}}));
    CHECK(r.status == OrderStatus::Filled);
    CHECK(!e.book().contains(1));
    CHECK_EQ(e.book().find(2)->remainingQuantity(), 20);
    CHECK(consistent(e.book()));
}

TEST(Scenario8_NoEligibleLiquidity) {
    MatchingEngine e;
    rest(e, sell(1, 110, 100));
    ExecutionReport r = e.submit(buy(2, 105, 100));
    CHECK(r.accepted);
    CHECK_EQ(r.tradeCount(), 0u);
    CHECK(r.status == OrderStatus::New);  // accepted, distinguishable from rejected
    CHECK(r.rejectReason == RejectReason::None);
    CHECK_EQ(r.executedQuantity, 0);
    CHECK_EQ(r.remainingQuantity, 100);
    CHECK(r.rested());
    CHECK_EQ(e.book().bestBid()->price, 105);
    CHECK_EQ(e.book().bestAsk()->price, 110);
    CHECK_EQ(e.book().find(1)->remainingQuantity(), 100);
    CHECK(e.trades().empty());
    CHECK(consistent(e.book()));
}

TEST(Scenario9_MultipleExecutions) {
    MatchingEngine e;
    rest(e, sell(1, 100, 20));
    rest(e, sell(2, 101, 30));
    rest(e, sell(3, 102, 40));
    ExecutionReport r = e.submit(buy(4, 105, 70));
    CHECK_EQ(r.tradeCount(), 3u);
    CHECK((execs(r) == std::vector<Exec>{{1, 4, 100, 20}, {2, 4, 101, 30}, {3, 4, 102, 20}}));
    Quantity total = 0;
    for (const Trade& t : r.trades) total += t.quantity();
    CHECK_EQ(total, 70);
    CHECK_EQ(r.executedQuantity, 70);
    CHECK(r.status == OrderStatus::Filled);
    CHECK_EQ(e.book().find(3)->remainingQuantity(), 20);
    CHECK(consistent(e.book()));
}

TEST(Scenario10_CrossedBookIsWorkedThrough) {
    // Build a crossed book directly through the OrderBook API.
    OrderBook crossed;
    crossed.add(buy(1, 110, 10));
    crossed.add(buy(2, 108, 10));
    crossed.add(sell(3, 100, 5));
    crossed.add(sell(4, 101, 5));
    CHECK(*crossed.spread() < 0);

    MatchingEngine e(std::move(crossed));
    CHECK(consistent(e.book()));
    // This sell trades against the best bids first (110, then 108) until done.
    ExecutionReport r = e.submit(sell(5, 100, 20));
    CHECK((execs(r) == std::vector<Exec>{{1, 5, 110, 10}, {2, 5, 108, 10}}));
    CHECK(r.status == OrderStatus::Filled);
    CHECK_EQ(e.book().bidLevelCount(), 0u);
    CHECK_EQ(e.book().askLevelCount(), 2u);
    CHECK(notCrossed(e.book()));  // normal state restored
    CHECK(consistent(e.book()));
}

TEST(Scenario10b_EngineDoesNotUncrossRestingOrdersOnItsOwn) {
    // Documented limitation: only incoming orders trade. A cross that exists
    // between resting orders is untouched by an order that does not reach it.
    OrderBook crossed;
    crossed.add(buy(1, 110, 10));
    crossed.add(sell(2, 100, 5));
    MatchingEngine e(std::move(crossed));
    ExecutionReport r = e.submit(buy(3, 50, 1));  // rests far from the cross
    CHECK_EQ(r.tradeCount(), 0u);
    CHECK(*e.book().spread() < 0);
    CHECK(consistent(e.book()));
}

// ---- Trade identities and ids ------------------------------------------

TEST(TradeIdentitiesForIncomingBuy) {
    MatchingEngine e;
    rest(e, sell(10, 105, 5));
    ExecutionReport r = e.submit(buy(20, 105, 5));
    const Trade& t = r.trades.at(0);
    CHECK_EQ(t.makerOrderId(), 10u);  // resting
    CHECK_EQ(t.takerOrderId(), 20u);  // incoming
    CHECK_EQ(t.buyOrderId(), 20u);
    CHECK_EQ(t.sellOrderId(), 10u);
    CHECK(t.takerSide() == Side::Buy);
}

TEST(TradeIdentitiesForIncomingSell) {
    MatchingEngine e;
    rest(e, buy(10, 105, 5));
    ExecutionReport r = e.submit(sell(20, 105, 5));
    const Trade& t = r.trades.at(0);
    CHECK_EQ(t.makerOrderId(), 10u);
    CHECK_EQ(t.takerOrderId(), 20u);
    CHECK_EQ(t.buyOrderId(), 10u);
    CHECK_EQ(t.sellOrderId(), 20u);
    CHECK(t.takerSide() == Side::Sell);
}

TEST(TradeIdsAreSequentialAndHistoryMatchesReports) {
    MatchingEngine e;
    for (OrderId i = 1; i <= 5; ++i) rest(e, sell(i, 100, 10));
    ExecutionReport r1 = e.submit(buy(100, 100, 25));  // 3 trades
    ExecutionReport r2 = e.submit(buy(101, 100, 25));  // 3 trades (5 + ...)
    CHECK_EQ(r1.tradeCount(), 3u);
    CHECK_EQ(e.trades().size(), r1.tradeCount() + r2.tradeCount());
    TradeId expected = 1;
    for (const Trade& t : e.trades()) CHECK_EQ(t.id(), expected++);
    CHECK_EQ(e.nextTradeId(), expected);
    CHECK_EQ(r1.trades[0].sequence(), r1.trades[2].sequence());  // taker's arrival sequence
}

TEST(CustomFirstTradeId) {
    MatchingEngine e(1000);
    rest(e, sell(1, 100, 5));
    CHECK_EQ(e.submit(buy(2, 100, 5)).trades.at(0).id(), 1000u);
    CHECK_THROWS(MatchingEngine(0), std::invalid_argument);
}

TEST(TradeIdOverflowIsRefusedBeforeAnyChange) {
    const TradeId max = std::numeric_limits<TradeId>::max();
    MatchingEngine e(max - 1);  // exactly two ids left: max-1 and max
    CHECK_EQ(e.tradeIdsRemaining(), 2u);
    for (OrderId i = 1; i <= 3; ++i) rest(e, sell(i, 100, 10));

    // Needs three executions but only two ids remain: refused, nothing changed.
    CHECK_THROWS(e.submit(buy(10, 100, 30)), std::overflow_error);
    CHECK_EQ(e.book().orderCount(), 3u);
    CHECK_EQ(e.book().find(1)->remainingQuantity(), 10);
    CHECK(!e.book().contains(10));
    CHECK(e.trades().empty());
    CHECK_EQ(e.tradeIdsRemaining(), 2u);
    CHECK(consistent(e.book()));

    // Needs two executions: fits exactly, uses max-1 and max.
    ExecutionReport r = e.submit(buy(11, 100, 20));
    CHECK_EQ(r.tradeCount(), 2u);
    CHECK_EQ(r.trades[0].id(), max - 1);
    CHECK_EQ(r.trades[1].id(), max);
    CHECK_EQ(e.tradeIdsRemaining(), 0u);

    // Exhausted: an order that would trade is refused, ids are never reused ...
    CHECK_THROWS(e.submit(buy(12, 100, 5)), std::overflow_error);
    CHECK_EQ(e.book().find(3)->remainingQuantity(), 10);
    // ... but an order that does not trade can still rest.
    ExecutionReport rested = e.submit(buy(13, 90, 5));
    CHECK(rested.accepted);
    CHECK_EQ(rested.tradeCount(), 0u);
    CHECK(consistent(e.book()));
}

// ---- Rejections ---------------------------------------------------------

TEST(RejectsInvalidRawInput) {
    MatchingEngine e;
    rest(e, sell(1, 100, 10));
    ExecutionReport a = e.submit(0, Side::Buy, 100, 5, ++g_seq);
    CHECK(!a.accepted);
    CHECK(a.rejectReason == RejectReason::InvalidOrderId);
    CHECK(a.status == OrderStatus::Rejected);
    ExecutionReport b = e.submit(2, Side::Buy, 0, 5, ++g_seq);
    CHECK(b.rejectReason == RejectReason::InvalidPrice);
    ExecutionReport c = e.submit(3, Side::Buy, -4, 5, ++g_seq);
    CHECK(c.rejectReason == RejectReason::InvalidPrice);
    ExecutionReport d = e.submit(4, Side::Buy, 100, 0, ++g_seq);
    CHECK(d.rejectReason == RejectReason::InvalidQuantity);
    ExecutionReport f = e.submit(5, Side::Buy, 100, -7, ++g_seq);
    CHECK(f.rejectReason == RejectReason::InvalidQuantity);
    CHECK_EQ(f.originalQuantity, -7);  // echoes the request
    CHECK_EQ(f.remainingQuantity, 0);
    CHECK_EQ(f.tradeCount(), 0u);
    // Book untouched; a valid order with the same raw API works.
    CHECK_EQ(e.book().orderCount(), 1u);
    CHECK_EQ(e.book().find(1)->remainingQuantity(), 10);
    CHECK(e.trades().empty());
    CHECK_EQ(e.nextTradeId(), 1u);
    ExecutionReport ok = e.submit(6, Side::Buy, 100, 4, ++g_seq);
    CHECK(ok.accepted);
    CHECK_EQ(ok.tradeCount(), 1u);
}

TEST(RejectsDuplicateIncomingIdAndLeavesBookUnchanged) {
    MatchingEngine e;
    rest(e, sell(1, 100, 10));
    rest(e, buy(2, 90, 10));
    ExecutionReport same = e.submit(buy(2, 100, 10));  // would have traded if accepted
    CHECK(!same.accepted);
    CHECK(same.rejectReason == RejectReason::DuplicateOrderId);
    CHECK_EQ(same.tradeCount(), 0u);
    ExecutionReport cross = e.submit(buy(1, 100, 10));  // id of a resting order on the other side
    CHECK(cross.rejectReason == RejectReason::DuplicateOrderId);
    CHECK_EQ(e.book().orderCount(), 2u);
    CHECK_EQ(e.book().find(1)->remainingQuantity(), 10);
    CHECK(e.trades().empty());
    CHECK(consistent(e.book()));
}

TEST(RejectsInactiveIncomingOrder) {
    MatchingEngine e;
    rest(e, sell(1, 100, 10));
    Order dead = buy(2, 100, 10);
    dead.cancel();
    ExecutionReport r = e.submit(dead);
    CHECK(!r.accepted);
    CHECK(r.rejectReason == RejectReason::OrderNotActive);
    CHECK_EQ(e.book().find(1)->remainingQuantity(), 10);
}

TEST(IdOfFullyFilledOrderIsNotRemembered) {
    // Policy inherited from the book: only resting ids are tracked.
    MatchingEngine e;
    rest(e, sell(1, 100, 10));
    CHECK(e.submit(buy(2, 100, 10)).accepted);
    CHECK(!e.book().contains(1));
    CHECK(e.submit(sell(1, 100, 10)).accepted);  // id 1 is free again
}

// ---- Edge cases ---------------------------------------------------------

TEST(EmptyBookAndOneSidedBook) {
    MatchingEngine e;
    ExecutionReport r = e.submit(buy(1, 100, 10));
    CHECK(r.accepted);
    CHECK_EQ(r.tradeCount(), 0u);
    CHECK(r.status == OrderStatus::New);
    // Only bids exist: another buy has nothing to hit; a far sell rests.
    CHECK_EQ(e.submit(buy(2, 101, 10)).tradeCount(), 0u);
    CHECK_EQ(e.submit(sell(3, 150, 10)).tradeCount(), 0u);
    CHECK_EQ(e.book().orderCount(), 3u);
    CHECK(consistent(e.book()));
}

TEST(MinimumQuantity) {
    MatchingEngine e;
    rest(e, sell(1, 100, 1));
    ExecutionReport r = e.submit(buy(2, 100, 1));
    CHECK((execs(r) == std::vector<Exec>{{1, 2, 100, 1}}));
    CHECK(e.book().empty());
}

TEST(LargeQuantitiesAndPrices) {
    const Quantity bigQty = 4'000'000'000'000'000'000;
    const Price bigPrice = std::numeric_limits<Price>::max() / 2;
    MatchingEngine e;
    rest(e, sell(1, bigPrice, bigQty));
    ExecutionReport r = e.submit(buy(2, std::numeric_limits<Price>::max(), bigQty - 1));
    CHECK_EQ(r.trades.at(0).price(), bigPrice);
    CHECK_EQ(r.trades.at(0).quantity(), bigQty - 1);
    CHECK_EQ(e.book().find(1)->remainingQuantity(), 1);
    CHECK(consistent(e.book()));
}

TEST(MultiplePartialFillsOfOneRestingOrder) {
    MatchingEngine e;
    rest(e, sell(1, 100, 100));
    rest(e, sell(2, 100, 10));
    for (OrderId i = 0; i < 3; ++i) {
        ExecutionReport r = e.submit(buy(10 + i, 100, 30));
        CHECK_EQ(r.trades.at(0).makerOrderId(), 1u);  // always the same, still-first order
        CHECK(r.status == OrderStatus::Filled);
    }
    CHECK_EQ(e.book().find(1)->remainingQuantity(), 10);
    CHECK(e.book().find(1)->status() == OrderStatus::PartiallyFilled);
    CHECK((idsAt(e.book().asks().at(100)) == std::vector<OrderId>{1, 2}));
    // Finishing it off moves on to the next order.
    ExecutionReport r = e.submit(buy(20, 100, 15));
    CHECK((execs(r) == std::vector<Exec>{{1, 20, 100, 10}, {2, 20, 100, 5}}));
    CHECK(consistent(e.book()));
}

TEST(IncomingLargerThanAllLiquidity) {
    MatchingEngine e;
    rest(e, sell(1, 100, 10));
    rest(e, sell(2, 101, 10));
    ExecutionReport r = e.submit(buy(3, 200, 50));
    CHECK_EQ(r.tradeCount(), 2u);
    CHECK_EQ(r.remainingQuantity, 30);
    CHECK(r.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(e.book().askLevelCount(), 0u);
    CHECK_EQ(e.book().bestBid()->price, 200);  // rests at its own limit, not at a trade price
    CHECK_EQ(e.book().bestBid()->totalQuantity, 30);
    CHECK(consistent(e.book()));
}

TEST(IncomingSmallerThanFirstRestingOrder) {
    MatchingEngine e;
    rest(e, sell(1, 100, 50));
    ExecutionReport r = e.submit(buy(2, 100, 5));
    CHECK_EQ(r.tradeCount(), 1u);
    CHECK_EQ(e.book().find(1)->remainingQuantity(), 45);
}

TEST(IncomingExactlyEqualsAllLiquidityAcrossLevels) {
    MatchingEngine e;
    rest(e, sell(1, 100, 10));
    rest(e, sell(2, 101, 20));
    rest(e, sell(3, 101, 30));
    ExecutionReport r = e.submit(buy(4, 101, 60));
    CHECK_EQ(r.tradeCount(), 3u);
    CHECK(r.status == OrderStatus::Filled);
    CHECK(e.book().empty());
    CHECK_EQ(e.book().askLevelCount(), 0u);
    CHECK_EQ(e.book().bidLevelCount(), 0u);
    CHECK(consistent(e.book()));
}

TEST(LevelAndLookupRemovedWhenLastOrderFills) {
    MatchingEngine e;
    rest(e, sell(1, 100, 10));
    rest(e, sell(2, 101, 10));
    e.submit(buy(3, 100, 10));
    CHECK(!e.book().contains(1));
    CHECK(e.book().find(1) == nullptr);
    CHECK_EQ(e.book().askLevelCount(), 1u);
    CHECK(e.book().asks().count(100) == 0);  // level erased
    CHECK_EQ(e.book().bestAsk()->price, 101);
    CHECK(consistent(e.book()));
}

TEST(RestedRemainderQueuesBehindExistingOrdersAtItsPrice) {
    MatchingEngine e;
    rest(e, buy(1, 100, 10));
    rest(e, sell(2, 101, 5));
    // Buy 100 @ 101 trades 5 against order 2, then rests the other 95 at its own limit, 101.
    ExecutionReport r = e.submit(buy(3, 101, 100));
    CHECK_EQ(r.remainingQuantity, 95);
    rest(e, buy(4, 101, 1));  // arrives later at the same price
    CHECK((idsAt(e.book().bids().at(101)) == std::vector<OrderId>{3, 4}));
    // And the earlier arrival at 101 is served first.
    ExecutionReport hit = e.submit(sell(5, 101, 96));
    CHECK((execs(hit) == std::vector<Exec>{{3, 5, 101, 95}, {4, 5, 101, 1}}));
    CHECK(consistent(e.book()));
}

TEST(RepeatedMatchingKeepsBookUncrossedAndConsistent) {
    MatchingEngine e;
    for (OrderId i = 1; i <= 20; ++i) {
        e.submit(i % 2 == 0 ? buy(i, 100 + static_cast<Price>(i % 5), 10)
                            : sell(i, 100 + static_cast<Price>(i % 5), 10));
        CHECK(notCrossed(e.book()));
        CHECK(consistent(e.book()));
    }
}

// ---- Reference-model stress ---------------------------------------------

namespace {

// Deliberately naive independent model: a flat list scanned linearly for the
// best eligible resting order each time. It shares no code with the engine.
struct RefOrder {
    OrderId id;
    Side side;
    Price price;
    Sequence seq;
    Quantity remaining;
};

struct RefBook {
    std::vector<RefOrder> orders;

    static bool better(Side side, Price a, Price b) {  // is a strictly better than b?
        return side == Side::Buy ? a > b : a < b;
    }

    std::vector<Exec> submit(OrderId id, Side side, Price price, Quantity qty, Sequence seq) {
        std::vector<Exec> out;
        const Side opp = opposite(side);
        while (qty > 0) {
            int best = -1;
            for (std::size_t i = 0; i < orders.size(); ++i) {
                const RefOrder& o = orders[i];
                if (o.side != opp) continue;
                const bool eligible = side == Side::Buy ? price >= o.price : price <= o.price;
                if (!eligible) continue;
                if (best < 0) { best = static_cast<int>(i); continue; }
                const RefOrder& b = orders[static_cast<std::size_t>(best)];
                if (better(opp, o.price, b.price) || (o.price == b.price && o.seq < b.seq)) {
                    best = static_cast<int>(i);
                }
            }
            if (best < 0) break;
            RefOrder& m = orders[static_cast<std::size_t>(best)];
            const Quantity q = std::min(qty, m.remaining);
            out.push_back({m.id, id, m.price, q});
            qty -= q;
            m.remaining -= q;
            if (m.remaining == 0) orders.erase(orders.begin() + best);
        }
        if (qty > 0) orders.push_back({id, side, price, seq, qty});
        return out;
    }
};

void runStress(std::uint64_t seed, int steps) {
    std::uint64_t state = seed;
    auto next = [&state](std::uint64_t bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return (state >> 33) % bound;
    };

    MatchingEngine engine;
    RefBook ref;
    std::size_t totalTrades = 0;

    for (int step = 0; step < steps; ++step) {
        const OrderId id = static_cast<OrderId>(step + 1);
        const Side side = next(2) == 0 ? Side::Buy : Side::Sell;
        const Price price = 95 + static_cast<Price>(next(11));  // narrow band: lots of crossing
        const Quantity qty = 1 + static_cast<Quantity>(next(40));
        const Sequence seq = ++g_seq;

        const std::vector<Exec> expected = ref.submit(id, side, price, qty, seq);
        auto order = Order::create(id, side, OrderType::Limit, price, qty, seq);
        const ExecutionReport r = engine.submit(*order);

        CHECK(r.accepted);
        CHECK((execs(r) == expected));
        Quantity traded = 0;
        for (const Exec& x : expected) traded += x.qty;
        CHECK_EQ(r.executedQuantity, traded);
        CHECK_EQ(r.remainingQuantity, qty - traded);
        CHECK(r.status == (traded == qty ? OrderStatus::Filled
                           : traded > 0 ? OrderStatus::PartiallyFilled : OrderStatus::New));
        for (const Trade& t : r.trades) {
            CHECK(t.takerOrderId() == id);
            CHECK(t.price() > 0 && t.quantity() > 0);
        }
        totalTrades += expected.size();

        CHECK(notCrossed(engine.book()));
        CHECK(consistent(engine.book()));
        CHECK_EQ(engine.book().orderCount(), ref.orders.size());
    }

    // Trade ids: unique, gapless, in execution order.
    CHECK_EQ(engine.trades().size(), totalTrades);
    TradeId expectedId = 1;
    for (const Trade& t : engine.trades()) CHECK_EQ(t.id(), expectedId++);

    // Final book state equals the model's, level by level, in priority order.
    auto snapshotRef = [&ref](Side s) {
        std::vector<RefOrder> v;
        for (const RefOrder& o : ref.orders) if (o.side == s) v.push_back(o);
        std::stable_sort(v.begin(), v.end(), [s](const RefOrder& a, const RefOrder& b) {
            if (a.price != b.price) return RefBook::better(s, a.price, b.price);
            return a.seq < b.seq;
        });
        return v;
    };
    auto compare = [](const std::vector<RefOrder>& want, const auto& sideMap) {
        std::size_t i = 0;
        bool same = true;
        for (const auto& level : sideMap) {
            for (const Order& o : level.second) {
                if (i >= want.size() || want[i].id != o.id() || want[i].price != o.price() ||
                    want[i].remaining != o.remainingQuantity()) {
                    same = false;
                }
                ++i;
            }
        }
        return same && i == want.size();
    };
    CHECK(compare(snapshotRef(Side::Buy), engine.book().bids()));
    CHECK(compare(snapshotRef(Side::Sell), engine.book().asks()));
}

}  // namespace

TEST(StressAgainstReferenceModelSeedA) { runStress(20240601, 3000); }
TEST(StressAgainstReferenceModelSeedB) { runStress(987654321, 3000); }

TEST(StressDeepBookWithSweeps) {
    // Build a deep two-sided book far from the touch, then sweep it with large orders.
    MatchingEngine e;
    RefBook ref;
    OrderId id = 0;
    for (int i = 0; i < 300; ++i) {
        ++id;
        const Price p = 200 + static_cast<Price>(i % 50);
        ref.submit(id, Side::Sell, p, 10, g_seq + 1);
        e.submit(sell(id, p, 10));
        ++id;
        const Price b = 150 - static_cast<Price>(i % 50);
        ref.submit(id, Side::Buy, b, 10, g_seq + 1);
        e.submit(buy(id, b, 10));
    }
    CHECK(consistent(e.book()));
    CHECK_EQ(e.book().orderCount(), 600u);
    for (int i = 0; i < 20; ++i) {
        ++id;
        const Side side = i % 2 == 0 ? Side::Buy : Side::Sell;
        const Price p = side == Side::Buy ? 230 : 120;
        const Sequence s = ++g_seq;
        const auto expected = ref.submit(id, side, p, 400, s);
        const auto r = e.submit(*Order::create(id, side, OrderType::Limit, p, 400, s));
        CHECK((execs(r) == expected));
        CHECK(consistent(e.book()));
        CHECK(notCrossed(e.book()));
    }
}

TEST_MAIN()
