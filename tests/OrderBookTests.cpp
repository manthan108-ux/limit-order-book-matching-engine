#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "OrderBook.h"
#include "TestFramework.h"

using namespace lob;

namespace {

Sequence g_seq = 0;  // arrival counter shared by the helpers: strictly increasing

Order mk(OrderId id, Side side, Price price, Quantity qty) {
    auto o = Order::create(id, side, OrderType::Limit, price, qty, ++g_seq);
    if (!o) throw std::runtime_error("test setup: order creation failed");
    return *o;
}

Order buy(OrderId id, Price price, Quantity qty) { return mk(id, Side::Buy, price, qty); }
Order sell(OrderId id, Price price, Quantity qty) { return mk(id, Side::Sell, price, qty); }

// Describes any violations so a failure explains itself in the test output.
bool consistent(const OrderBook& book) {
    const InvariantReport r = book.checkInvariants();
    for (const auto& v : r.violations) std::cerr << "    invariant violated: " << v << "\n";
    return r.ok();
}

std::vector<OrderId> idsAt(const PriceLevel& level) {
    std::vector<OrderId> out;
    for (const Order& o : level) out.push_back(o.id());
    return out;
}

std::vector<Price> bidPrices(const OrderBook& b) {
    std::vector<Price> p;
    for (const auto& kv : b.bids()) p.push_back(kv.first);
    return p;
}

std::vector<Price> askPrices(const OrderBook& b) {
    std::vector<Price> p;
    for (const auto& kv : b.asks()) p.push_back(kv.first);
    return p;
}

}  // namespace

// ---- Basic operations ---------------------------------------------------

TEST(BookStartsEmpty) {
    OrderBook book;
    CHECK(book.empty());
    CHECK_EQ(book.orderCount(), 0u);
    CHECK_EQ(book.bidLevelCount(), 0u);
    CHECK_EQ(book.askLevelCount(), 0u);
    CHECK(!book.bestBid().has_value());
    CHECK(!book.bestAsk().has_value());
    CHECK(!book.spread().has_value());
    CHECK(!book.midPrice().has_value());
    CHECK(book.depth(5).bids.empty());
    CHECK(book.depth(5).asks.empty());
    CHECK(!book.contains(1));
    CHECK(book.find(1) == nullptr);
    CHECK(consistent(book));
}

TEST(BookAddSingleBuy) {
    OrderBook book;
    CHECK(book.add(buy(1, 100, 10)) == RejectReason::None);
    CHECK_EQ(book.orderCount(), 1u);
    CHECK_EQ(book.bidLevelCount(), 1u);
    CHECK_EQ(book.askLevelCount(), 0u);
    CHECK(book.contains(1));
    CHECK(!book.empty());
    auto bid = book.bestBid();
    CHECK(bid.has_value());
    CHECK_EQ(bid->price, 100);
    CHECK_EQ(bid->totalQuantity, 10);
    CHECK_EQ(bid->orderCount, 1u);
    CHECK(!book.bestAsk().has_value());
    CHECK(consistent(book));
}

TEST(BookAddSingleSell) {
    OrderBook book;
    CHECK(book.add(sell(1, 105, 7)) == RejectReason::None);
    CHECK_EQ(book.askLevelCount(), 1u);
    CHECK_EQ(book.bidLevelCount(), 0u);
    CHECK_EQ(book.bestAsk()->price, 105);
    CHECK_EQ(book.bestAsk()->totalQuantity, 7);
    CHECK(!book.bestBid().has_value());
    CHECK(consistent(book));
}

TEST(BookFindReturnsRestingOrder) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    const Order* o = book.find(1);
    CHECK(o != nullptr);
    if (o != nullptr) {
        CHECK_EQ(o->id(), 1u);
        CHECK_EQ(o->price(), 100);
        CHECK(o->side() == Side::Buy);
        CHECK_EQ(o->remainingQuantity(), 10);
    }
    CHECK(book.find(2) == nullptr);
}

TEST(BookMultipleOrdersSamePrice) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(buy(2, 100, 20));
    book.add(buy(3, 100, 30));
    CHECK_EQ(book.bidLevelCount(), 1u);
    CHECK_EQ(book.orderCount(), 3u);
    CHECK_EQ(book.bestBid()->totalQuantity, 60);
    CHECK_EQ(book.bestBid()->orderCount, 3u);
    CHECK(consistent(book));
}

TEST(BookMultipleOrdersDifferentPrices) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(buy(2, 99, 20));
    book.add(sell(3, 101, 5));
    book.add(sell(4, 102, 6));
    CHECK_EQ(book.bidLevelCount(), 2u);
    CHECK_EQ(book.askLevelCount(), 2u);
    CHECK_EQ(book.orderCount(), 4u);
    CHECK(consistent(book));
}

// ---- Price ordering and FIFO -------------------------------------------

TEST(BookBidsOrderedHighestFirst) {
    OrderBook book;
    // Inserted deliberately out of order: middle, low, high.
    book.add(buy(1, 100, 1));
    book.add(buy(2, 90, 1));
    book.add(buy(3, 110, 1));
    CHECK((bidPrices(book) == std::vector<Price>{110, 100, 90}));
    CHECK_EQ(book.bestBid()->price, 110);
    CHECK(consistent(book));
}

TEST(BookAsksOrderedLowestFirst) {
    OrderBook book;
    book.add(sell(1, 100, 1));
    book.add(sell(2, 90, 1));
    book.add(sell(3, 110, 1));
    CHECK((askPrices(book) == std::vector<Price>{90, 100, 110}));
    CHECK_EQ(book.bestAsk()->price, 90);
    CHECK(consistent(book));
}

TEST(BookFifoWithinLevel) {
    OrderBook book;
    book.add(buy(30, 100, 1));
    book.add(buy(10, 100, 1));
    book.add(buy(20, 100, 1));
    // Arrival order, not id order.
    CHECK((idsAt(book.bids().at(100)) == std::vector<OrderId>{30, 10, 20}));
    // A later order at the same price goes behind, even if another level exists.
    book.add(buy(40, 101, 1));
    book.add(buy(50, 100, 1));
    CHECK((idsAt(book.bids().at(100)) == std::vector<OrderId>{30, 10, 20, 50}));
    CHECK(consistent(book));
}

// ---- ID management ------------------------------------------------------

TEST(BookRejectsDuplicateIdSameSide) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    CHECK(book.add(buy(1, 101, 5)) == RejectReason::DuplicateOrderId);
    CHECK(book.add(buy(1, 100, 5)) == RejectReason::DuplicateOrderId);
    CHECK_EQ(book.orderCount(), 1u);
    CHECK_EQ(book.bidLevelCount(), 1u);          // no level created for the rejected add
    CHECK_EQ(book.bestBid()->totalQuantity, 10);  // original untouched
    CHECK(consistent(book));
}

TEST(BookRejectsDuplicateIdAcrossSides) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    CHECK(book.add(sell(1, 105, 5)) == RejectReason::DuplicateOrderId);
    CHECK_EQ(book.askLevelCount(), 0u);
    CHECK_EQ(book.orderCount(), 1u);
    CHECK(consistent(book));
}

TEST(BookRejectsInactiveOrders) {
    OrderBook book;
    Order cancelled = buy(1, 100, 10);
    cancelled.cancel();
    CHECK(book.add(cancelled) == RejectReason::OrderNotActive);
    Order filled = sell(2, 100, 10);
    filled.fill(10);
    CHECK(book.add(filled) == RejectReason::OrderNotActive);
    CHECK(book.empty());
    CHECK(consistent(book));
}

TEST(BookAcceptsPartiallyFilledOrderUsingRemainingQuantity) {
    OrderBook book;
    Order o = buy(1, 100, 10);
    o.fill(4);
    CHECK(book.add(o) == RejectReason::None);
    CHECK_EQ(book.bestBid()->totalQuantity, 6);
    CHECK(consistent(book));
}

TEST(BookUnknownLookupAndCancel) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    CHECK(!book.contains(99));
    CHECK(book.find(99) == nullptr);
    CHECK(!book.cancel(99).has_value());
    CHECK_EQ(book.orderCount(), 1u);
    CHECK(consistent(book));
}

TEST(BookRepeatedCancelIsSafe) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    CHECK(book.cancel(1).has_value());
    CHECK(!book.cancel(1).has_value());
    CHECK(!book.cancel(1).has_value());
    CHECK(book.empty());
    CHECK(consistent(book));
}

TEST(BookIdCanBeReusedAfterCancel) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.cancel(1);
    // Policy: the book only tracks resting ids, so a cancelled id is free again,
    // even on the other side and at another price.
    CHECK(book.add(sell(1, 120, 3)) == RejectReason::None);
    CHECK(book.find(1)->side() == Side::Sell);
    CHECK_EQ(book.bidLevelCount(), 0u);
    CHECK(consistent(book));
}

// ---- Cancellation -------------------------------------------------------

TEST(BookCancelReturnsCancelledOrder) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    auto o = book.cancel(1);
    CHECK(o.has_value());
    CHECK_EQ(o->id(), 1u);
    CHECK(o->status() == OrderStatus::Cancelled);
    CHECK_EQ(o->remainingQuantity(), 10);
    CHECK(!book.contains(1));
    CHECK(book.find(1) == nullptr);
}

TEST(BookCancelFirstInLevel) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(buy(2, 100, 20));
    book.add(buy(3, 100, 30));
    CHECK(book.cancel(1).has_value());
    CHECK((idsAt(book.bids().at(100)) == std::vector<OrderId>{2, 3}));
    CHECK_EQ(book.bestBid()->totalQuantity, 50);
    CHECK(consistent(book));
}

TEST(BookCancelMiddleInLevel) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(buy(2, 100, 20));
    book.add(buy(3, 100, 30));
    CHECK(book.cancel(2).has_value());
    CHECK((idsAt(book.bids().at(100)) == std::vector<OrderId>{1, 3}));  // priority kept
    CHECK_EQ(book.bestBid()->totalQuantity, 40);
    CHECK_EQ(book.bestBid()->orderCount, 2u);
    CHECK(consistent(book));
}

TEST(BookCancelLastInLevel) {
    OrderBook book;
    book.add(sell(1, 100, 10));
    book.add(sell(2, 100, 20));
    book.add(sell(3, 100, 30));
    CHECK(book.cancel(3).has_value());
    CHECK((idsAt(book.asks().at(100)) == std::vector<OrderId>{1, 2}));
    CHECK_EQ(book.bestAsk()->totalQuantity, 30);
    CHECK(consistent(book));
}

TEST(BookCancelOnlyOrderErasesLevel) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(buy(2, 99, 10));
    book.add(sell(3, 105, 10));
    CHECK(book.cancel(1).has_value());
    CHECK_EQ(book.bidLevelCount(), 1u);
    CHECK((bidPrices(book) == std::vector<Price>{99}));
    CHECK_EQ(book.bestBid()->price, 99);  // best bid falls back to the next level
    CHECK(book.cancel(3).has_value());
    CHECK_EQ(book.askLevelCount(), 0u);
    CHECK(!book.bestAsk().has_value());
    CHECK(consistent(book));
}

TEST(BookCancelOrderAtDeepLevel) {
    OrderBook book;
    for (OrderId i = 1; i <= 10; ++i) book.add(buy(i, 100 - static_cast<Price>(i), 5));
    CHECK(book.cancel(7).has_value());  // nowhere near the best price
    CHECK_EQ(book.bidLevelCount(), 9u);
    CHECK_EQ(book.bestBid()->price, 99);
    CHECK(consistent(book));
}

TEST(BookLevelCanBeRecreatedAfterErase) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.cancel(1);
    CHECK(book.add(buy(2, 100, 4)) == RejectReason::None);
    CHECK_EQ(book.bestBid()->totalQuantity, 4);
    CHECK_EQ(book.bestBid()->orderCount, 1u);
    CHECK(consistent(book));
}

TEST(BookCancelledIdsOnlyAffectTheirOwnEntries) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(buy(2, 100, 10));
    book.cancel(1);
    CHECK(book.contains(2));
    CHECK(book.find(2) != nullptr);
    CHECK_EQ(book.find(2)->id(), 2u);
    CHECK(consistent(book));
}

// ---- Market data --------------------------------------------------------

TEST(BookSpread) {
    OrderBook book;
    book.add(buy(1, 100, 1));
    CHECK(!book.spread().has_value());  // one side only
    book.add(sell(2, 105, 1));
    CHECK(book.spread().has_value());
    CHECK_EQ(*book.spread(), 5);
}

TEST(BookSpreadWhileCrossedIsNotPositive) {
    // Matching does not exist yet, so a crossed book can be represented.
    OrderBook book;
    book.add(buy(1, 105, 1));
    book.add(sell(2, 100, 1));
    CHECK_EQ(*book.spread(), -5);
    book.add(sell(3, 105, 1));
    book.cancel(2);
    CHECK_EQ(*book.spread(), 0);
    CHECK(consistent(book));
}

TEST(BookMidPriceEvenDifference) {
    OrderBook book;
    book.add(buy(1, 100, 1));
    book.add(sell(2, 104, 1));
    auto mid = book.midPrice();
    CHECK(mid.has_value());
    CHECK_EQ(mid->ticks, 102);
    CHECK(!mid->halfTick);
}

TEST(BookMidPriceOddDifference) {
    OrderBook book;
    book.add(buy(1, 100, 1));
    book.add(sell(2, 105, 1));
    auto mid = book.midPrice();  // 102.5
    CHECK_EQ(mid->ticks, 102);
    CHECK(mid->halfTick);

    OrderBook other;
    other.add(buy(1, 101, 1));
    other.add(sell(2, 104, 1));  // 102.5 again, from odd + even
    CHECK_EQ(other.midPrice()->ticks, 102);
    CHECK(other.midPrice()->halfTick);
}

TEST(BookMidPriceBothOddPricesIsExact) {
    OrderBook book;
    book.add(buy(1, 101, 1));
    book.add(sell(2, 103, 1));  // (101 + 103) / 2 = 102 exactly
    CHECK_EQ(book.midPrice()->ticks, 102);
    CHECK(!book.midPrice()->halfTick);
}

TEST(BookMidPriceDoesNotOverflowAtExtremePrices) {
    const Price big = std::numeric_limits<Price>::max();  // odd
    OrderBook book;
    book.add(buy(1, big - 1, 1));
    book.add(sell(2, big, 1));
    // bid + ask would overflow Price; the midpoint is big - 1/2.
    auto mid = book.midPrice();
    CHECK_EQ(mid->ticks, big - 1);  // floor(big - 0.5)
    CHECK(mid->halfTick);
    CHECK_EQ(*book.spread(), 1);
}

TEST(BookDepthTopN) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(buy(2, 100, 5));
    book.add(buy(3, 99, 7));
    book.add(buy(4, 98, 1));
    book.add(sell(5, 101, 3));
    book.add(sell(6, 102, 4));
    book.add(sell(7, 102, 6));
    book.add(sell(8, 103, 2));

    MarketDepth d = book.depth(2);
    CHECK_EQ(d.bids.size(), 2u);
    CHECK_EQ(d.bids[0].price, 100);
    CHECK_EQ(d.bids[0].totalQuantity, 15);
    CHECK_EQ(d.bids[0].orderCount, 2u);
    CHECK_EQ(d.bids[1].price, 99);
    CHECK_EQ(d.bids[1].totalQuantity, 7);
    CHECK_EQ(d.asks.size(), 2u);
    CHECK_EQ(d.asks[0].price, 101);
    CHECK_EQ(d.asks[1].price, 102);
    CHECK_EQ(d.asks[1].totalQuantity, 10);
    CHECK_EQ(d.asks[1].orderCount, 2u);
}

TEST(BookDepthMoreThanAvailableAndZero) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(sell(2, 101, 3));
    book.add(sell(3, 102, 3));
    MarketDepth d = book.depth(10);
    CHECK_EQ(d.bids.size(), 1u);
    CHECK_EQ(d.asks.size(), 2u);
    MarketDepth none = book.depth(0);
    CHECK(none.bids.empty());
    CHECK(none.asks.empty());
}

TEST(BookDepthDoesNotModifyBook) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(sell(2, 101, 3));
    (void)book.depth(5);
    (void)book.bestBid();
    (void)book.midPrice();
    CHECK_EQ(book.orderCount(), 2u);
    CHECK(consistent(book));
}

TEST(BookCountsTrackOperations) {
    OrderBook book;
    book.add(buy(1, 100, 1));
    book.add(buy(2, 100, 1));
    book.add(buy(3, 99, 1));
    book.add(sell(4, 101, 1));
    CHECK_EQ(book.orderCount(), 4u);
    CHECK_EQ(book.bidLevelCount(), 2u);
    CHECK_EQ(book.askLevelCount(), 1u);
    book.cancel(1);
    book.cancel(3);
    CHECK_EQ(book.orderCount(), 2u);
    CHECK_EQ(book.bidLevelCount(), 1u);
}

// ---- Integrity ----------------------------------------------------------

TEST(BookInvariantsHoldOnCrossedBook) {
    // An OrderBook used on its own does not match, so it can hold a crossed book; it is still consistent.
    OrderBook book;
    book.add(buy(1, 110, 5));
    book.add(sell(2, 100, 5));
    CHECK(consistent(book));
}

TEST(BookInvariantsAfterMixedOperations) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    CHECK(consistent(book));
    book.add(buy(2, 100, 10));
    book.add(sell(3, 105, 10));
    CHECK(consistent(book));
    book.cancel(1);
    CHECK(consistent(book));
    book.add(buy(1, 98, 4));  // reuse of a cancelled id
    book.add(sell(4, 105, 6));
    book.cancel(3);
    book.cancel(77);          // unknown
    CHECK(book.add(buy(2, 50, 1)) == RejectReason::DuplicateOrderId);
    CHECK(consistent(book));
    CHECK_EQ(book.orderCount(), 3u);
}

TEST(BookMoveKeepsHandlesValid) {
    OrderBook a;
    a.add(buy(1, 100, 10));
    a.add(buy(2, 100, 20));
    a.add(sell(3, 105, 5));
    OrderBook b(std::move(a));
    CHECK(consistent(b));
    CHECK(b.cancel(1).has_value());  // goes through a handle created before the move
    CHECK_EQ(b.bestBid()->totalQuantity, 20);
    CHECK(consistent(b));
}

// ---- fill() and bestOrder() (used by the matching engine) -----

TEST(BookBestOrderIsOldestAtBestPrice) {
    OrderBook book;
    CHECK(book.bestOrder(Side::Buy) == nullptr);
    CHECK(book.bestOrder(Side::Sell) == nullptr);
    book.add(buy(1, 99, 1));
    book.add(buy(2, 100, 1));
    book.add(buy(3, 100, 1));
    book.add(sell(4, 105, 1));
    book.add(sell(5, 104, 1));
    book.add(sell(6, 104, 1));
    CHECK_EQ(book.bestOrder(Side::Buy)->id(), 2u);   // best price, then oldest
    CHECK_EQ(book.bestOrder(Side::Sell)->id(), 5u);
    book.cancel(2);
    CHECK_EQ(book.bestOrder(Side::Buy)->id(), 3u);
    book.cancel(3);
    CHECK_EQ(book.bestOrder(Side::Buy)->id(), 1u);   // falls to the next level
}

TEST(BookPartialFillKeepsPositionAndLookup) {
    OrderBook book;
    book.add(sell(1, 100, 10));
    book.add(sell(2, 100, 10));
    CHECK(book.fill(1, 4) == PriceLevel::FillResult::PartiallyFilled);
    CHECK(book.contains(1));
    CHECK_EQ(book.find(1)->remainingQuantity(), 6);
    CHECK(book.find(1)->status() == OrderStatus::PartiallyFilled);
    CHECK_EQ(book.bestOrder(Side::Sell)->id(), 1u);  // still first in line
    CHECK_EQ(book.bestAsk()->totalQuantity, 16);
    CHECK(consistent(book));
}

TEST(BookCompleteFillRemovesOrderLookupAndEmptyLevel) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(buy(2, 99, 10));
    CHECK(book.fill(1, 10) == PriceLevel::FillResult::Filled);
    CHECK(!book.contains(1));
    CHECK(book.find(1) == nullptr);
    CHECK_EQ(book.bidLevelCount(), 1u);
    CHECK(book.bids().count(100) == 0);
    CHECK_EQ(book.bestBid()->price, 99);
    CHECK_EQ(book.orderCount(), 1u);
    CHECK(consistent(book));
    CHECK(!book.cancel(1).has_value());  // nothing stale left to cancel
}

TEST(BookFillInMiddleOfQueueLeavesNeighboursIntact) {
    OrderBook book;
    book.add(sell(1, 100, 5));
    book.add(sell(2, 100, 5));
    book.add(sell(3, 100, 5));
    book.fill(2, 5);
    CHECK((idsAt(book.asks().at(100)) == std::vector<OrderId>{1, 3}));
    CHECK(book.cancel(3).has_value());  // handle of a neighbour still valid
    CHECK_EQ(book.bestAsk()->totalQuantity, 5);
    CHECK(consistent(book));
}

TEST(BookFillRejectsUnknownIdAndBadQuantityWithoutChange) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    CHECK_THROWS(book.fill(99, 1), std::out_of_range);
    CHECK_THROWS(book.fill(1, 0), std::invalid_argument);
    CHECK_THROWS(book.fill(1, -2), std::invalid_argument);
    CHECK_THROWS(book.fill(1, 11), std::invalid_argument);
    CHECK_EQ(book.find(1)->remainingQuantity(), 10);
    CHECK_EQ(book.bestBid()->totalQuantity, 10);
    CHECK(consistent(book));
}

TEST(BookDeterministicStressMatchesReferenceModel) {
    // Pseudo-random but fully deterministic (fixed-seed LCG) mix of adds and cancels,
    // checked against an independent, deliberately naive model.
    std::uint64_t state = 12345;
    auto next = [&state](std::uint64_t bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return (state >> 33) % bound;
    };

    struct Live { Side side; Price price; Quantity qty; };
    std::map<OrderId, Live> model;
    OrderBook book;
    OrderId nextId = 1;

    for (int step = 0; step < 6000; ++step) {
        const bool doCancel = !model.empty() && next(100) < 40;
        if (doCancel) {
            // Cancel a pseudo-randomly chosen live order (any depth, any position).
            auto it = model.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(next(model.size())));
            CHECK(book.cancel(it->first).has_value());
            CHECK(!book.cancel(it->first).has_value());  // repeat is a no-op
            model.erase(it);
        } else {
            const Side side = next(2) == 0 ? Side::Buy : Side::Sell;
            const Price price = 90 + static_cast<Price>(next(21));
            const Quantity qty = 1 + static_cast<Quantity>(next(50));
            const OrderId id = nextId++;
            CHECK(book.add(mk(id, side, price, qty)) == RejectReason::None);
            model[id] = Live{side, price, qty};
        }
        if (step % 500 == 0) CHECK(consistent(book));
    }
    CHECK(consistent(book));
    CHECK_EQ(book.orderCount(), model.size());

    // Rebuild per-(side, price) aggregates from the model and compare to the book.
    std::map<std::pair<Side, Price>, std::pair<Quantity, std::size_t>> expected;
    for (const auto& kv : model) {
        auto& e = expected[{kv.second.side, kv.second.price}];
        e.first += kv.second.qty;
        e.second += 1;
        CHECK(book.contains(kv.first));
    }
    std::size_t levels = 0;
    for (const auto& kv : book.bids()) {
        const auto& e = expected.at({Side::Buy, kv.first});
        CHECK_EQ(kv.second.totalQuantity(), e.first);
        CHECK_EQ(kv.second.orderCount(), e.second);
        ++levels;
    }
    for (const auto& kv : book.asks()) {
        const auto& e = expected.at({Side::Sell, kv.first});
        CHECK_EQ(kv.second.totalQuantity(), e.first);
        CHECK_EQ(kv.second.orderCount(), e.second);
        ++levels;
    }
    CHECK_EQ(levels, expected.size());  // no stray levels, none missing

    // Drain everything: the book must return to a pristine empty state.
    for (const auto& kv : model) CHECK(book.cancel(kv.first).has_value());
    CHECK(book.empty());
    CHECK_EQ(book.bidLevelCount(), 0u);
    CHECK_EQ(book.askLevelCount(), 0u);
    CHECK(consistent(book));
}


// ---- extract() and reduce() (used by order modification) ------------------

TEST(BookExtractReturnsOrderUnchangedAndFreesTheId) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(buy(2, 100, 10));
    book.fill(1, 4);
    auto o = book.extract(1);
    CHECK(o.has_value());
    CHECK_EQ(o->remainingQuantity(), 6);
    CHECK(o->status() == OrderStatus::PartiallyFilled);  // not cancelled
    CHECK(!book.contains(1));
    CHECK((idsAt(book.bids().at(100)) == std::vector<OrderId>{2}));
    CHECK_EQ(book.bestBid()->totalQuantity, 10);
    CHECK(consistent(book));
    CHECK(!book.extract(1).has_value());  // gone
    // Re-entering with a fresh sequence (as the engine does) is allowed: the id is free.
    CHECK(book.add(o->replaced(100, o->remainingQuantity(), ++g_seq)) == RejectReason::None);
    CHECK((idsAt(book.bids().at(100)) == std::vector<OrderId>{2, 1}));  // at the back
    CHECK(consistent(book));
}

TEST(BookExtractErasesEmptyLevel) {
    OrderBook book;
    book.add(sell(1, 100, 10));
    book.extract(1);
    CHECK(book.empty());
    CHECK_EQ(book.askLevelCount(), 0u);
    CHECK(consistent(book));
}

TEST(BookReduceKeepsPositionAndAggregate) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    book.add(buy(2, 100, 20));
    book.add(buy(3, 100, 30));
    book.reduce(2, 5);
    CHECK((idsAt(book.bids().at(100)) == std::vector<OrderId>{1, 2, 3}));
    CHECK_EQ(book.find(2)->remainingQuantity(), 5);
    CHECK_EQ(book.bestBid()->totalQuantity, 45);
    CHECK(book.contains(2));
    CHECK(consistent(book));
}

TEST(BookReduceRejectsBadInputWithoutChange) {
    OrderBook book;
    book.add(sell(1, 100, 10));
    CHECK_THROWS(book.reduce(99, 1), std::out_of_range);
    CHECK_THROWS(book.reduce(1, 0), std::invalid_argument);
    CHECK_THROWS(book.reduce(1, 10), std::invalid_argument);
    CHECK_THROWS(book.reduce(1, 20), std::invalid_argument);
    CHECK_EQ(book.find(1)->remainingQuantity(), 10);
    CHECK_EQ(book.bestAsk()->totalQuantity, 10);
    CHECK(consistent(book));
}

TEST(BookCancelStillMarksOrderCancelledAfterRefactor) {
    OrderBook book;
    book.add(buy(1, 100, 10));
    auto o = book.cancel(1);
    CHECK(o->status() == OrderStatus::Cancelled);
    CHECK(!book.cancel(1).has_value());
}

TEST_MAIN()
