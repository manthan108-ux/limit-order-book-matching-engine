// Part 1 checks the reference model ITSELF against hand-computed expectations (an
// oracle nobody has checked is not an oracle). Part 2 runs deterministic scenarios
// through production and the model side by side (Duel) and requires them to agree on
// every report, every order, the book and the trade history, with the system auditor
// running after every operation.
#include <limits>
#include <string>
#include <vector>

#include "Duel.h"
#include "ReferenceExchange.h"
#include "TestFramework.h"

using namespace lob;

namespace {

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

struct T {
    OrderId maker;
    OrderId taker;
    Price price;
    Quantity qty;
};

bool tradesAre(const std::vector<ref::Trade>& got, const std::vector<T>& want) {
    if (got.size() != want.size()) return false;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i].maker != want[i].maker || got[i].taker != want[i].taker || got[i].price != want[i].price ||
            got[i].quantity != want[i].qty) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ===== Part 1: the model against hand-computed answers =====================================

TEST(ModelExecutesAtTheRestingPrice) {
    ref::Exchange x;
    x.submit(1, S, 105, 100);
    const auto e = x.submit(2, B, 110, 100);
    CHECK(tradesAre(e.trades, {{1, 2, 105, 100}}));  // 105, not the buyer's 110
    CHECK(e.status == OrderStatus::Filled);
    CHECK_EQ(x.activeCount(), 0u);
    ref::Exchange y;
    y.submit(1, B, 100, 100);
    CHECK(tradesAre(y.submit(2, S, 95, 100).trades, {{1, 2, 100, 100}}));  // 100, not 95
}

TEST(ModelHonoursPriceThenTimePriority) {
    ref::Exchange x;
    x.submit(1, S, 105, 30);  // A
    x.submit(2, S, 105, 40);  // B (later, same price)
    x.submit(3, S, 103, 50);  // C (better price, latest)
    const auto e = x.submit(4, B, 105, 100);
    // C first (best price), then A (oldest at 105), then B.
    CHECK(tradesAre(e.trades, {{3, 4, 103, 50}, {1, 4, 105, 30}, {2, 4, 105, 20}}));
    CHECK_EQ(x.orders().at(2).remaining, 20);
    CHECK(x.orders().at(4).status == OrderStatus::Filled);
}

TEST(ModelRestsTheUnmatchedRemainderAtItsOwnPrice) {
    ref::Exchange x;
    x.submit(1, S, 100, 10);
    const auto e = x.submit(2, B, 105, 25);
    CHECK(tradesAre(e.trades, {{1, 2, 100, 10}}));
    CHECK(e.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(e.remaining, 15);
    const auto bids = x.levels(B);
    CHECK_EQ(bids.size(), 1u);
    CHECK_EQ(bids[0].price, 105);   // its own limit, not a trade price
    CHECK_EQ(bids[0].total, 15);
}

TEST(ModelStopsAtTheLimitPrice) {
    ref::Exchange x;
    x.submit(1, S, 103, 30);
    x.submit(2, S, 105, 40);
    x.submit(3, S, 108, 50);
    const auto e = x.submit(4, B, 106, 100);
    CHECK(tradesAre(e.trades, {{1, 4, 103, 30}, {2, 4, 105, 40}}));
    CHECK_EQ(e.remaining, 30);
    CHECK_EQ(x.orders().at(3).remaining, 50);  // 108 untouched
}

TEST(ModelModificationPriorityRules) {
    ref::Exchange x;
    x.submit(1, B, 100, 10);
    x.submit(2, B, 100, 10);
    x.submit(3, B, 100, 10);
    x.modify(1, std::nullopt, 4);   // decrease: keeps its place (front)
    x.modify(2, std::nullopt, 25);  // increase: to the back
    // A SELL now hits order 1, then 3, then 2.
    const auto e = x.submit(9, S, 100, 100);
    CHECK(tradesAre(e.trades, {{1, 9, 100, 4}, {3, 9, 100, 10}, {2, 9, 100, 25}}));
}

TEST(ModelModifyKeepsExecutedQuantityAndEffectiveTotals) {
    ref::Exchange x;
    x.submit(1, B, 100, 100);
    x.submit(2, S, 100, 40);        // executes 40 of order 1
    x.modify(1, std::nullopt, 30);  // remaining 60 -> 30
    CHECK_EQ(x.orders().at(1).executed, 40);
    CHECK_EQ(x.orders().at(1).effective, 70);
    x.modify(1, std::nullopt, 80);  // remaining 30 -> 80
    CHECK_EQ(x.orders().at(1).executed, 40);
    CHECK_EQ(x.orders().at(1).effective, 120);
    CHECK_EQ(x.orders().at(1).remaining, 80);
}

TEST(ModelCancelAndTerminalStates) {
    ref::Exchange x;
    x.submit(1, B, 100, 50);
    x.submit(2, S, 100, 20);
    const auto c = x.cancel(1);
    CHECK(c.accepted);
    CHECK_EQ(x.orders().at(1).cancelled, 30);
    CHECK_EQ(x.orders().at(1).executed, 20);
    CHECK_EQ(x.orders().at(1).remaining, 0);
    CHECK(x.cancel(1).reason == RejectReason::OrderNotActive);
    CHECK(x.cancel(2).reason == RejectReason::OrderNotActive);  // filled
    CHECK(x.cancel(99).reason == RejectReason::OrderNotFound);
    CHECK(x.modify(1, 5, 5).reason == RejectReason::OrderNotActive);
}

TEST(ModelIdPolicy) {
    ref::Exchange x;
    CHECK(x.submit(0, B, 100, 5).reason == RejectReason::InvalidOrderId);
    CHECK(x.submit(7, B, 100, 5).accepted);
    CHECK(x.submit(7, S, 90, 5).reason == RejectReason::DuplicateOrderId);
    CHECK(x.submit(8, B, 0, 5).reason == RejectReason::InvalidPrice);     // recorded + retired
    CHECK(x.submit(8, B, 100, 5).reason == RejectReason::DuplicateOrderId);
    CHECK(x.submit(9, B, 100, -1).reason == RejectReason::InvalidQuantity);
    // Generated ids skip every id already used.
    CHECK_EQ(x.submitGenerated(B, 90, 1).orderId, 1u);
    CHECK_EQ(x.submitGenerated(B, 90, 1).orderId, 2u);
    ref::Exchange y(5);
    y.submit(5, B, 90, 1);
    CHECK_EQ(y.submitGenerated(B, 90, 1).orderId, 6u);
}

TEST(ModelGeneratedIdsNeverWrap) {
    const OrderId max = std::numeric_limits<OrderId>::max();
    ref::Exchange x(max - 1);
    CHECK_EQ(x.submitGenerated(B, 90, 1).orderId, max - 1);
    CHECK_EQ(x.submitGenerated(B, 90, 1).orderId, max);
    CHECK(x.submitGenerated(B, 90, 1).reason == RejectReason::IdSpaceExhausted);
}

TEST(ModelMarketDataArithmetic) {
    ref::Exchange x;
    x.submit(1, B, 100, 10);
    x.submit(2, B, 100, 5);
    x.submit(3, B, 99, 7);
    x.submit(4, S, 105, 3);
    CHECK_EQ(x.levels(B).size(), 2u);
    CHECK_EQ(x.levels(B)[0].price, 100);  // best bid first
    CHECK_EQ(x.levels(B)[0].total, 15);
    CHECK_EQ(x.levels(B)[0].count, 2u);
    CHECK_EQ(*x.spread(), 5);
    CHECK_EQ(x.mid()->floorTicks, 102u);  // (100 + 105) / 2 = 102.5
    CHECK(x.mid()->half);

    ref::Exchange z;
    const Price big = std::numeric_limits<Price>::max();
    z.submit(1, B, big - 1, 1);
    z.submit(2, S, big, 1);
    CHECK_EQ(z.mid()->floorTicks, static_cast<std::uint64_t>(big - 1));  // big - 0.5, floored
    CHECK(z.mid()->half);
}

// ===== Part 2: production vs model, deterministic scenarios ================================

TEST(DuelPriceImprovementAndRestingPriceExecution) {
    Duel d("price-improvement");
    d.submit(1, S, 105, 100);
    d.submit(2, B, 110, 100);   // executes at 105
    d.submit(3, B, 100, 100);
    d.submit(4, S, 95, 100);    // executes at 100
    d.checkEverything();
    CHECK_MSG(d.clean(), d.failure());
}

TEST(DuelFifoAndPartialFillsAtOnePrice) {
    Duel d("fifo");
    d.submit(1, S, 105, 30);
    d.submit(2, S, 105, 40);
    d.submit(3, S, 105, 50);
    d.submit(4, B, 105, 60);    // fills 1, half of 2
    d.submit(5, B, 105, 25);    // rest of 2 (10), then 15 of 3
    d.submit(6, B, 105, 100);   // 35 of 3, remainder rests
    d.checkEverything();
    CHECK_MSG(d.clean(), d.failure());
}

TEST(DuelMultiLevelSweepsStopAtTheLimit) {
    Duel d("multi-level");
    d.submit(1, S, 103, 30);
    d.submit(2, S, 105, 40);
    d.submit(3, S, 108, 50);
    d.submit(4, B, 106, 100);   // 103, 105, then rest 30 @106
    d.submit(5, B, 99, 10);
    d.submit(6, B, 99, 10);
    d.submit(7, S, 99, 35);     // hits 106 first (best bid), then 99s in FIFO order
    d.checkEverything();
    CHECK_MSG(d.clean(), d.failure());
}

TEST(DuelIncomingSellsAgainstBids) {
    Duel d("sell-side");
    d.submit(1, B, 105, 30);
    d.submit(2, B, 103, 40);
    d.submit(3, S, 103, 50);    // 105 first, then 103
    d.submit(4, B, 104, 5);
    d.submit(5, S, 200, 10);    // does not cross: rests
    d.checkEverything();
    CHECK_MSG(d.clean(), d.failure());
}

TEST(DuelCancelPartialAndModifyLifecycle) {
    Duel d("lifecycle");
    d.submit(1, B, 100, 100);
    d.submit(2, S, 100, 40);      // order 1: 40 executed
    d.modify(1, std::nullopt, 30);  // decrease, keeps priority
    d.submit(3, B, 100, 10);
    d.modify(1, std::nullopt, 80);  // increase: to the back
    d.modify(3, 101, std::nullopt); // price change
    d.cancel(1);
    d.cancel(1);                    // repeated cancel
    d.cancel(2);                    // filled
    d.modify(1, 5, 5);              // terminal
    d.modify(404, 5, 5);            // unknown
    d.modify(3, 101, std::nullopt); // no change
    d.checkEverything();
    CHECK_MSG(d.clean(), d.failure());
}

TEST(DuelModifyIntoTheBookTradesLikeANewOrder) {
    Duel d("modify-cross");
    d.submit(1, S, 105, 10);
    d.submit(2, S, 106, 10);
    d.submit(3, B, 100, 15);
    d.modify(3, 106, std::nullopt);   // crosses both levels
    d.submit(4, B, 90, 20);
    d.modify(4, 105, 20);             // crosses nothing now (asks are gone)
    d.checkEverything();
    CHECK_MSG(d.clean(), d.failure());
}

TEST(DuelRejectionsAndIdPolicy) {
    Duel d("ids");
    d.submit(0, B, 100, 5);        // invalid id
    d.submit(7, B, 100, 5);
    d.submit(7, S, 90, 5);         // duplicate active id (would have traded)
    d.submit(8, B, 0, 5);          // rejected, retired
    d.submit(8, B, 100, 5);        // reuse of a rejected id
    d.submit(9, B, 100, -3);
    d.cancel(7);
    d.submit(7, B, 100, 5);        // reuse after cancel
    d.submit(S, 100, 10);          // generated ids skip used ones
    d.submit(S, 100, 10);
    d.checkEverything();
    CHECK_MSG(d.clean(), d.failure());
}

TEST(DuelCrossingBothWaysEmptiesTheBookRepeatedly) {
    Duel d("ping-pong");
    for (int i = 0; i < 40; ++i) {
        d.submit(i % 2 == 0 ? B : S, 100, 10);   // alternating, always exactly crossing
    }
    CHECK_EQ(d.manager.book().orderCount(), 0u);
    d.checkEverything();
    CHECK_MSG(d.clean(), d.failure());
}

TEST(DuelOneBigOrderAgainstManySmallOnes) {
    Duel d("hub");
    for (int i = 1; i <= 30; ++i) d.submit(static_cast<OrderId>(i), S, 100 + (i % 4), 3);
    d.submit(100, B, 110, 200);       // sweeps everything, rests the remainder
    d.checkEverything();
    CHECK_MSG(d.clean(), d.failure());
    CHECK_EQ(d.manager.tradesFor(100).size(), 30u);
}

TEST(DuelDisagreementsAreReported) {
    // The harness must fail loudly when production and model differ. Feed the model an
    // operation production never sees, then compare.
    Duel d("sanity");
    d.submit(1, B, 100, 10);
    CHECK(d.clean());
    d.model.submit(2, S, 100, 10);   // only the model trades
    std::cerr << "    (the next MISMATCH line is expected: it proves the harness detects divergence)\n";
    d.checkEverything("deliberate divergence");
    CHECK(!d.clean());
}

TEST_MAIN()
