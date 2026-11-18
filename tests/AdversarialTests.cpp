// Long, deterministic, adversarial operation sequences. Each is run as a
// production-vs-model duel with the system auditor, so both the book and the
// lifecycle/trade history are verified, not just the final state.
#include <cstdint>
#include <string>
#include <vector>

#include "Duel.h"
#include "TestFramework.h"
#include "TestRng.h"

using namespace lob;

namespace {

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

#define DUEL_OK(d) CHECK_MSG((d).clean(), (d).failure())

}  // namespace

TEST(ChurnAtASinglePriceLevel) {
    Duel d("churn", 0, 200);
    OrderId id = 1;
    for (int i = 0; i < 3000; ++i) {
        d.submit(id, B, 100, 1 + i % 7);
        if (i % 3 != 0) d.cancel(id);   // two of three are cancelled immediately
        ++id;
    }
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.book().bidLevelCount(), 1u);
}

TEST(OscillationBetweenAnEmptyAndNonEmptyBook) {
    Duel d("oscillate", 0, 200);
    for (int i = 0; i < 3000; ++i) {
        d.submit(i % 2 == 0 ? B : S, 100, 5);   // the second of each pair cancels the first out exactly
        if (i % 2 == 1) CHECK_EQ(d.manager.book().orderCount(), 0u);
    }
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.trades().size(), 1500u);
}

TEST(RepeatedlyRecreatingAndRemovingTheBestLevel) {
    Duel d("best-level", 0, 200);
    OrderId id = 1;
    d.submit(id++, B, 90, 5);                      // a permanent far bid, so the book is never empty
    for (int i = 0; i < 1500; ++i) {
        const Price p = 100 + (i % 5);
        const OrderId a = id++;
        d.submit(a, B, p, 3);                      // creates a new best level
        CHECK_EQ(d.manager.book().bestBid()->price, p > 90 ? p : 90);
        if (i % 2 == 0) d.cancel(a);               // removed by cancellation...
        else d.submit(id++, S, p, 3);              // ...or by a trade
    }
    d.checkEverything();
    DUEL_OK(d);
}

TEST(RepeatedModificationsOfOneOrderAmongCompetitors) {
    Duel d("repeated-modify", 0, 100);
    for (OrderId i = 1; i <= 6; ++i) d.submit(i, B, 100, 10);        // a queue to compete with
    for (int i = 0; i < 1500; ++i) {
        switch (i % 6) {
            case 0: d.modify(1, std::nullopt, 3 + i % 11); break;     // may shrink (keeps place) or grow (loses it)
            case 1: d.modify(1, 100 + (i % 3), std::nullopt); break;  // price moves
            case 2: d.modify(1, 100, 10 + i % 5); break;
            case 3: d.modify(2, std::nullopt, 1 + i % 4); break;
            case 4: d.modify(1, std::nullopt, std::nullopt); break;   // no-change (must be harmless)
            default: d.modify(3, 99 + (i % 4), std::nullopt); break;
        }
    }
    d.submit(100, S, 90, 1000);                                       // everyone is hit, in whatever order they ended up
    d.checkEverything();
    DUEL_OK(d);
}

TEST(OrdersThatPartiallyExecuteAreThenCancelled) {
    Duel d("partial-then-cancel", 0, 100);
    OrderId id = 1;
    for (int round = 0; round < 400; ++round) {
        const OrderId big = id++;
        d.submit(big, B, 100, 50);
        d.submit(id++, S, 100, 1 + round % 20);   // partially executes it
        d.cancel(big);                            // cancel the unfilled remainder
        d.cancel(big);                            // and again: must be a no-op rejection
    }
    d.checkEverything();
    DUEL_OK(d);
    CHECK(d.manager.book().empty());
}

TEST(OrdersThatPartiallyExecuteAreThenModified) {
    Duel d("partial-then-modify", 0, 100);
    OrderId id = 1;
    for (int round = 0; round < 300; ++round) {
        const OrderId maker = id++;
        d.submit(maker, S, 100, 40);
        d.submit(id++, B, 100, 10 + round % 15);         // partial fill of the maker
        switch (round % 4) {
            case 0: d.modify(maker, std::nullopt, 5); break;       // shrink what is left
            case 1: d.modify(maker, std::nullopt, 80); break;      // grow it (loses priority)
            case 2: d.modify(maker, 101, std::nullopt); break;     // reprice it
            default: d.modify(maker, 99, 7); break;                // reprice and resize: crosses the book? (only if bids exist)
        }
        d.submit(id++, B, 105, 100);                      // sweeps whatever is left of it
    }
    d.checkEverything();
    DUEL_OK(d);
}

TEST(ManyOrdersCompetingAtIdenticalPrices) {
    Duel d("same-price", 0, 100);
    TestRng rng(5);
    OrderId id = 1;
    for (int i = 0; i < 400; ++i) d.submit(id++, B, 100, 1 + static_cast<Quantity>(rng.below(5)));
    for (int i = 0; i < 400; ++i) d.submit(id++, S, 100, 1 + static_cast<Quantity>(rng.below(5)));
    for (int i = 0; i < 200; ++i) d.submit(id++, i % 2 ? B : S, 100, 1 + static_cast<Quantity>(rng.below(5)));
    d.checkEverything();
    DUEL_OK(d);
}

TEST(AlternatingBuysAndSellsAcrossAMovingMarket) {
    Duel d("moving-market", 0, 100);
    Price mid = 500;
    for (int i = 0; i < 2500; ++i) {
        if (i % 50 == 0) mid += (i / 50) % 2 == 0 ? 3 : -2;
        const bool buy = i % 2 == 0;
        d.submit(buy ? B : S, buy ? mid - 1 + (i % 3) : mid + 1 - (i % 3), 1 + i % 9);
    }
    d.checkEverything();
    DUEL_OK(d);
}

TEST(OneHubOrderTradesWithHundredsOfCounterparties) {
    Duel d("hub", 0, 100);
    OrderId id = 1;
    for (int i = 0; i < 300; ++i) d.submit(id++, S, 100 + (i % 5), 2);   // 300 small makers
    const OrderId hubBuy = id++;
    d.submit(hubBuy, B, 110, 500);                                       // one taker hits 250 of them (500 / 2)
    const OrderId hubSell = id++;
    d.submit(hubSell, S, 90, 100000);                                    // a big maker that rests
    for (int i = 0; i < 200; ++i) d.submit(id++, B, 95, 3);              // many small takers against it
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.tradesFor(hubBuy).size(), 250u);
    CHECK_EQ(d.manager.tradesFor(hubSell).size(), 200u);
    // The hub's history is strictly in execution order with no duplicates (auditor C2/C5 also checked this).
    TradeId last = 0;
    for (const Trade& t : d.manager.tradesFor(hubSell)) {
        CHECK(t.id() > last);
        last = t.id();
    }
}

TEST(ThousandsOfTerminalRecordsStayConsistent) {
    Duel d("terminal-records", 0, 1500);
    OrderId id = 1;
    for (int i = 0; i < 2000; ++i) {
        d.submit(id, B, 100, 4);
        d.submit(id + 1, S, 100, 4);       // both FILLED
        d.submit(id + 2, B, 90, 4);
        d.cancel(id + 2);                  // CANCELLED
        d.submit(id + 3, S, 0, 4);         // REJECTED
        id += 4;
    }
    d.submit(id++, B, 80, 5);              // a few survivors
    d.submit(id++, S, 120, 5);
    d.checkEverything();
    DUEL_OK(d);
    CHECK_EQ(d.manager.recordedOrderCount(), 8002u);
    CHECK_EQ(d.manager.activeOrderCount(), 2u);
    // Every id ever used stays reserved.
    CHECK(d.manager.submit(1, B, 100, 1).rejectReason == RejectReason::DuplicateOrderId);
    CHECK(d.manager.submit(4, S, 100, 1).rejectReason == RejectReason::DuplicateOrderId);
}

TEST(ScriptedLifecycleCombinationsRepeatedManyTimes) {
    // Every ordering of {rest, partial fill, modify, cancel, resubmit-with-new-id} on a fresh order.
    Duel d("script", 0, 100);
    OrderId id = 1;
    for (int round = 0; round < 200; ++round) {
        const OrderId a = id++;
        d.submit(a, B, 100, 20);
        switch (round % 5) {
            case 0: d.submit(id++, S, 100, 5); d.modify(a, std::nullopt, 8); d.cancel(a); break;
            case 1: d.modify(a, 101, std::nullopt); d.submit(id++, S, 101, 30); d.cancel(a); break;
            case 2: d.cancel(a); d.modify(a, 100, 5); d.submit(a, B, 100, 5); break;
            case 3: d.submit(id++, S, 100, 20); d.cancel(a); d.modify(a, 100, 5); break;
            default: d.modify(a, std::nullopt, 40); d.modify(a, std::nullopt, 10); d.submit(id++, S, 95, 15); break;
        }
    }
    d.checkEverything();
    DUEL_OK(d);
}

TEST_MAIN()
