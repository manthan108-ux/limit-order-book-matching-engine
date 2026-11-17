#include <limits>
#include <type_traits>

#include "Trade.h"
#include "TestFramework.h"

using namespace lob;

TEST(TradeValidCreation) {
    auto t = Trade::create(1, 10, 20, Side::Buy, 100, 5, 9);
    CHECK(t.has_value());
    CHECK_EQ(t->id(), 1u);
    CHECK_EQ(t->makerOrderId(), 10u);
    CHECK_EQ(t->takerOrderId(), 20u);
    CHECK(t->takerSide() == Side::Buy);
    CHECK_EQ(t->price(), 100);
    CHECK_EQ(t->quantity(), 5);
    CHECK_EQ(t->sequence(), 9u);
}

TEST(TradeBuyAndSellOrderTraceability) {
    // Taker is the buyer: maker was the resting seller.
    auto buyTaker = Trade::create(1, 10, 20, Side::Buy, 100, 5, 1);
    CHECK_EQ(buyTaker->buyOrderId(), 20u);
    CHECK_EQ(buyTaker->sellOrderId(), 10u);
    // Taker is the seller: maker was the resting buyer.
    auto sellTaker = Trade::create(2, 10, 20, Side::Sell, 100, 5, 2);
    CHECK_EQ(sellTaker->buyOrderId(), 10u);
    CHECK_EQ(sellTaker->sellOrderId(), 20u);
}

TEST(TradeRejectsInvalidIds) {
    CHECK(!Trade::create(0, 10, 20, Side::Buy, 100, 5, 1).has_value());  // trade id
    CHECK(!Trade::create(1, 0, 20, Side::Buy, 100, 5, 1).has_value());   // maker id
    CHECK(!Trade::create(1, 10, 0, Side::Buy, 100, 5, 1).has_value());   // taker id
}

TEST(TradeRejectsIdenticalMakerAndTaker) {
    CHECK(!Trade::create(1, 10, 10, Side::Buy, 100, 5, 1).has_value());
    CHECK(!Trade::isValid(1, 10, 10, 100, 5));
}

TEST(TradeRejectsInvalidPrice) {
    CHECK(!Trade::create(1, 10, 20, Side::Buy, 0, 5, 1).has_value());
    CHECK(!Trade::create(1, 10, 20, Side::Buy, -100, 5, 1).has_value());
}

TEST(TradeRejectsInvalidQuantity) {
    CHECK(!Trade::create(1, 10, 20, Side::Buy, 100, 0, 1).has_value());
    CHECK(!Trade::create(1, 10, 20, Side::Buy, 100, -5, 1).has_value());
}

TEST(TradeBoundaryValues) {
    CHECK(Trade::isValid(1, 1, 2, 1, 1));
    CHECK(Trade::isValid(std::numeric_limits<TradeId>::max(),
                         std::numeric_limits<OrderId>::max(), 1,
                         std::numeric_limits<Price>::max(),
                         std::numeric_limits<Quantity>::max()));
}

TEST(TradeIsImmutableAndCopyable) {
    // No default construction, and construction is only possible through create().
    CHECK(!std::is_default_constructible<Trade>::value);
    CHECK(std::is_copy_constructible<Trade>::value);
    auto t = Trade::create(1, 10, 20, Side::Sell, 100, 5, 1);
    Trade copy = *t;
    CHECK_EQ(copy.id(), t->id());
    CHECK_EQ(copy.quantity(), t->quantity());
}

TEST_MAIN()
