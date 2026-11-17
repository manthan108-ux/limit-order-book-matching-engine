#include <stdexcept>
#include <vector>

#include "Order.h"
#include "PriceLevel.h"
#include "TestFramework.h"

using namespace lob;

namespace {
constexpr Price kPrice = 100;

Order makeOrder(OrderId id, Quantity qty, Price price = kPrice,
                Side side = Side::Buy) {
    auto o = Order::create(id, side, OrderType::Limit, price, qty, id);
    if (!o) throw std::runtime_error("test setup: order creation failed");
    return *o;
}

std::vector<OrderId> ids(const PriceLevel& level) {
    std::vector<OrderId> out;
    for (const Order& o : level) out.push_back(o.id());
    return out;
}
}  // namespace

TEST(PriceLevelRejectsInvalidPrice) {
    CHECK_THROWS(PriceLevel(0), std::invalid_argument);
    CHECK_THROWS(PriceLevel(-5), std::invalid_argument);
}

TEST(PriceLevelEmpty) {
    PriceLevel level(kPrice);
    CHECK_EQ(level.price(), kPrice);
    CHECK(level.empty());
    CHECK_EQ(level.orderCount(), 0u);
    CHECK_EQ(level.totalQuantity(), 0);
    CHECK(level.begin() == level.end());
    CHECK(level.checkInvariants());
}

TEST(PriceLevelEmptyOperationsThrow) {
    PriceLevel level(kPrice);
    CHECK_THROWS(level.front(), std::logic_error);
    CHECK_THROWS(level.popFront(), std::logic_error);
    CHECK_THROWS(level.remove(level.end()), std::out_of_range);
    CHECK_THROWS(level.fill(level.end(), 1), std::out_of_range);
    CHECK(level.empty());
    CHECK_EQ(level.totalQuantity(), 0);
}

TEST(PriceLevelSingleInsertion) {
    PriceLevel level(kPrice);
    auto h = level.add(makeOrder(1, 10));
    CHECK(!level.empty());
    CHECK_EQ(level.orderCount(), 1u);
    CHECK_EQ(level.totalQuantity(), 10);
    CHECK_EQ(level.front().id(), 1u);
    CHECK_EQ(h->id(), 1u);
    CHECK(level.checkInvariants());
}

TEST(PriceLevelMultipleInsertionAndFifo) {
    PriceLevel level(kPrice);
    level.add(makeOrder(1, 10));
    level.add(makeOrder(2, 20));
    level.add(makeOrder(3, 30));
    CHECK_EQ(level.orderCount(), 3u);
    CHECK_EQ(level.totalQuantity(), 60);
    CHECK((ids(level) == std::vector<OrderId>{1, 2, 3}));
    CHECK_EQ(level.front().id(), 1u);
    CHECK(level.checkInvariants());
}

TEST(PriceLevelNewOrderNeverBypassesExisting) {
    PriceLevel level(kPrice);
    level.add(makeOrder(5, 1));
    level.add(makeOrder(2, 1));  // smaller id, but arrived later: still behind
    CHECK((ids(level) == std::vector<OrderId>{5, 2}));
}

TEST(PriceLevelPopFrontFollowsFifo) {
    PriceLevel level(kPrice);
    level.add(makeOrder(1, 10));
    level.add(makeOrder(2, 20));
    level.add(makeOrder(3, 30));

    Order first = level.popFront();
    CHECK_EQ(first.id(), 1u);
    CHECK_EQ(level.front().id(), 2u);
    CHECK_EQ(level.totalQuantity(), 50);

    Order second = level.popFront();
    CHECK_EQ(second.id(), 2u);
    CHECK_EQ(level.totalQuantity(), 30);
    CHECK(level.checkInvariants());
}

TEST(PriceLevelRemoveMiddleByHandle) {
    PriceLevel level(kPrice);
    level.add(makeOrder(1, 10));
    auto mid = level.add(makeOrder(2, 20));
    level.add(makeOrder(3, 30));

    Order removed = level.remove(mid);
    CHECK_EQ(removed.id(), 2u);
    CHECK_EQ(removed.remainingQuantity(), 20);
    CHECK(removed.status() == OrderStatus::New);  // remove() does not alter status
    CHECK((ids(level) == std::vector<OrderId>{1, 3}));  // order of the rest is kept
    CHECK_EQ(level.totalQuantity(), 40);
    CHECK(level.checkInvariants());
}

TEST(PriceLevelRemoveFrontAndBackByHandle) {
    PriceLevel level(kPrice);
    auto a = level.add(makeOrder(1, 10));
    level.add(makeOrder(2, 20));
    auto c = level.add(makeOrder(3, 30));

    level.remove(c);
    CHECK((ids(level) == std::vector<OrderId>{1, 2}));
    level.remove(a);
    CHECK((ids(level) == std::vector<OrderId>{2}));
    CHECK_EQ(level.totalQuantity(), 20);
    CHECK(level.checkInvariants());
}

TEST(PriceLevelRemoveFinalOrderLeavesEmptyLevel) {
    PriceLevel level(kPrice);
    auto h = level.add(makeOrder(1, 10));
    level.remove(h);
    CHECK(level.empty());
    CHECK_EQ(level.orderCount(), 0u);
    CHECK_EQ(level.totalQuantity(), 0);
    CHECK(level.checkInvariants());
    // A level can be reused after it empties.
    level.add(makeOrder(2, 5));
    CHECK_EQ(level.totalQuantity(), 5);
}

TEST(PriceLevelHandlesStableAfterUnrelatedRemovals) {
    PriceLevel level(kPrice);
    auto h1 = level.add(makeOrder(1, 10));
    auto h2 = level.add(makeOrder(2, 20));
    auto h3 = level.add(makeOrder(3, 30));
    auto h4 = level.add(makeOrder(4, 40));

    level.remove(h2);
    level.popFront();  // removes order 1 (h1), unrelated to h3 / h4
    (void)h1;          // h1 is now invalid and must not be used again
    CHECK_EQ(h3->id(), 3u);
    CHECK_EQ(h4->id(), 4u);

    // Appending does not disturb existing handles either.
    level.add(makeOrder(5, 50));
    CHECK_EQ(h3->id(), 3u);

    // Removal through a handle that survived several other removals is still correct.
    Order removed = level.remove(h4);
    CHECK_EQ(removed.id(), 4u);
    CHECK((ids(level) == std::vector<OrderId>{3, 5}));
    CHECK_EQ(level.totalQuantity(), 80);
    CHECK(level.checkInvariants());
}

TEST(PriceLevelPartialFillUpdatesAggregate) {
    PriceLevel level(kPrice);
    auto h = level.add(makeOrder(1, 10));
    level.add(makeOrder(2, 20));

    auto result = level.fill(h, 4);
    CHECK(result == PriceLevel::FillResult::PartiallyFilled);
    CHECK_EQ(level.front().remainingQuantity(), 6);
    CHECK(level.front().status() == OrderStatus::PartiallyFilled);
    CHECK_EQ(level.totalQuantity(), 26);
    CHECK_EQ(level.orderCount(), 2u);
    CHECK_EQ(level.front().id(), 1u);  // a partial fill keeps time priority
    CHECK(level.checkInvariants());
}

TEST(PriceLevelFullFillRemovesOrder) {
    PriceLevel level(kPrice);
    auto h = level.add(makeOrder(1, 10));
    level.add(makeOrder(2, 20));

    auto result = level.fill(h, 10);
    CHECK(result == PriceLevel::FillResult::Filled);
    CHECK_EQ(level.orderCount(), 1u);
    CHECK_EQ(level.front().id(), 2u);
    CHECK_EQ(level.totalQuantity(), 20);
    CHECK(level.checkInvariants());
}

TEST(PriceLevelFillInStagesUntilFilled) {
    PriceLevel level(kPrice);
    auto h = level.add(makeOrder(1, 10));
    CHECK(level.fill(h, 3) == PriceLevel::FillResult::PartiallyFilled);
    CHECK(level.fill(h, 3) == PriceLevel::FillResult::PartiallyFilled);
    CHECK_EQ(level.totalQuantity(), 4);
    CHECK(level.fill(h, 4) == PriceLevel::FillResult::Filled);
    CHECK(level.empty());
    CHECK_EQ(level.totalQuantity(), 0);
}

TEST(PriceLevelFillWithBadQuantityLeavesLevelUnchanged) {
    PriceLevel level(kPrice);
    auto h = level.add(makeOrder(1, 10));
    CHECK_THROWS(level.fill(h, 0), std::invalid_argument);
    CHECK_THROWS(level.fill(h, -3), std::invalid_argument);
    CHECK_THROWS(level.fill(h, 11), std::invalid_argument);
    CHECK_EQ(level.totalQuantity(), 10);
    CHECK_EQ(level.front().remainingQuantity(), 10);
    CHECK(level.checkInvariants());
}

TEST(PriceLevelRejectsWrongPriceOrInactiveOrders) {
    PriceLevel level(kPrice);
    CHECK_THROWS(level.add(makeOrder(1, 10, kPrice + 1)), std::invalid_argument);

    Order cancelled = makeOrder(2, 10);
    cancelled.cancel();
    CHECK_THROWS(level.add(cancelled), std::invalid_argument);

    Order filled = makeOrder(3, 10);
    filled.fill(10);
    CHECK_THROWS(level.add(filled), std::invalid_argument);

    CHECK(level.empty());
    CHECK_EQ(level.totalQuantity(), 0);
}

TEST(PriceLevelAcceptsPartiallyFilledOrder) {
    // Needed when an incoming order partially executes and the remainder rests.
    PriceLevel level(kPrice);
    Order o = makeOrder(1, 10);
    o.fill(4);
    level.add(o);
    CHECK_EQ(level.totalQuantity(), 6);  // aggregate uses remaining, not original
    CHECK(level.checkInvariants());
}

TEST(PriceLevelAggregateNeverNegativeThroughChurn) {
    PriceLevel level(kPrice);
    std::vector<PriceLevel::OrderHandle> handles;
    for (OrderId i = 1; i <= 50; ++i) handles.push_back(level.add(makeOrder(i, 7)));
    CHECK_EQ(level.totalQuantity(), 350);
    // Remove every other order from the middle, partially fill the survivors.
    for (std::size_t i = 0; i < handles.size(); i += 2) level.remove(handles[i]);
    for (std::size_t i = 1; i < handles.size(); i += 2) level.fill(handles[i], 3);
    CHECK_EQ(level.orderCount(), 25u);
    CHECK_EQ(level.totalQuantity(), 25 * 4);
    CHECK(level.checkInvariants());
    while (!level.empty()) {
        level.popFront();
        CHECK(level.totalQuantity() >= 0);
    }
    CHECK_EQ(level.totalQuantity(), 0);
}

TEST_MAIN()
