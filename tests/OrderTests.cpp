#include <limits>
#include <stdexcept>

#include "Order.h"
#include "TestFramework.h"

using namespace lob;

namespace {
Order makeOrder(Quantity qty = 10) {
    auto o = Order::create(1, Side::Buy, OrderType::Limit, 100, qty, 7);
    if (!o) throw std::runtime_error("test setup: order creation failed");
    return *o;
}
}  // namespace

TEST(OrderValidCreation) {
    auto o = Order::create(42, Side::Sell, OrderType::Limit, 105, 25, 3);
    CHECK(o.has_value());
    CHECK_EQ(o->id(), 42u);
    CHECK(o->side() == Side::Sell);
    CHECK(o->type() == OrderType::Limit);
    CHECK_EQ(o->price(), 105);
    CHECK_EQ(o->originalQuantity(), 25);
    CHECK_EQ(o->remainingQuantity(), 25);
    CHECK_EQ(o->executedQuantity(), 0);
    CHECK_EQ(o->sequence(), 3u);
    CHECK(o->status() == OrderStatus::New);
    CHECK(o->isActive());
}

TEST(OrderRejectsZeroId) {
    CHECK(!Order::create(0, Side::Buy, OrderType::Limit, 100, 10, 1).has_value());
    CHECK(Order::validate(0, 100, 10) == RejectReason::InvalidOrderId);
}

TEST(OrderRejectsNonPositivePrice) {
    CHECK(!Order::create(1, Side::Buy, OrderType::Limit, 0, 10, 1).has_value());
    CHECK(!Order::create(1, Side::Buy, OrderType::Limit, -1, 10, 1).has_value());
    CHECK(!Order::create(1, Side::Buy, OrderType::Limit,
                         std::numeric_limits<Price>::min(), 10, 1).has_value());
    CHECK(Order::validate(1, 0, 10) == RejectReason::InvalidPrice);
    CHECK(Order::validate(1, -5, 10) == RejectReason::InvalidPrice);
}

TEST(OrderRejectsNonPositiveQuantity) {
    CHECK(!Order::create(1, Side::Buy, OrderType::Limit, 100, 0, 1).has_value());
    CHECK(!Order::create(1, Side::Buy, OrderType::Limit, 100, -10, 1).has_value());
    CHECK(Order::validate(1, 100, 0) == RejectReason::InvalidQuantity);
    CHECK(Order::validate(1, 100, -1) == RejectReason::InvalidQuantity);
}

TEST(OrderValidateBoundariesAndPrecedence) {
    CHECK(Order::validate(1, 1, 1) == RejectReason::None);
    CHECK(Order::validate(std::numeric_limits<OrderId>::max(),
                          std::numeric_limits<Price>::max(),
                          std::numeric_limits<Quantity>::max()) == RejectReason::None);
    // Everything wrong: the id is reported first, then price, then quantity.
    CHECK(Order::validate(0, 0, 0) == RejectReason::InvalidOrderId);
    CHECK(Order::validate(1, 0, 0) == RejectReason::InvalidPrice);
}

TEST(OrderPartialFill) {
    Order o = makeOrder(10);
    o.fill(4);
    CHECK_EQ(o.remainingQuantity(), 6);
    CHECK_EQ(o.executedQuantity(), 4);
    CHECK_EQ(o.originalQuantity(), 10);
    CHECK(o.status() == OrderStatus::PartiallyFilled);
    CHECK(o.isActive());
    o.fill(5);
    CHECK_EQ(o.remainingQuantity(), 1);
    CHECK(o.status() == OrderStatus::PartiallyFilled);
}

TEST(OrderFullFillInOneStep) {
    Order o = makeOrder(10);
    o.fill(10);
    CHECK_EQ(o.remainingQuantity(), 0);
    CHECK_EQ(o.executedQuantity(), 10);
    CHECK(o.status() == OrderStatus::Filled);
    CHECK(!o.isActive());
}

TEST(OrderFullFillAfterPartial) {
    Order o = makeOrder(10);
    o.fill(3);
    o.fill(7);
    CHECK_EQ(o.remainingQuantity(), 0);
    CHECK(o.status() == OrderStatus::Filled);
}

TEST(OrderFillRejectsBadQuantityWithoutChange) {
    Order o = makeOrder(10);
    CHECK_THROWS(o.fill(0), std::invalid_argument);
    CHECK_THROWS(o.fill(-1), std::invalid_argument);
    CHECK_THROWS(o.fill(11), std::invalid_argument);  // overfill
    // None of the failed calls may have changed anything.
    CHECK_EQ(o.remainingQuantity(), 10);
    CHECK(o.status() == OrderStatus::New);
}

TEST(OrderCancelFromNewAndPartial) {
    Order a = makeOrder(10);
    a.cancel();
    CHECK(a.status() == OrderStatus::Cancelled);
    CHECK(!a.isActive());
    CHECK_EQ(a.remainingQuantity(), 10);  // cancellation does not erase quantity

    Order b = makeOrder(10);
    b.fill(4);
    b.cancel();
    CHECK(b.status() == OrderStatus::Cancelled);
    CHECK_EQ(b.executedQuantity(), 4);
}

TEST(OrderRejectOnlyFromNew) {
    Order a = makeOrder();
    a.reject();
    CHECK(a.status() == OrderStatus::Rejected);

    Order b = makeOrder();
    b.fill(1);
    CHECK_THROWS(b.reject(), std::logic_error);
    CHECK(b.status() == OrderStatus::PartiallyFilled);
}

TEST(OrderIllegalTransitionsFromTerminalStates) {
    Order filled = makeOrder(5);
    filled.fill(5);
    CHECK_THROWS(filled.cancel(), std::logic_error);
    CHECK_THROWS(filled.reject(), std::logic_error);
    CHECK_THROWS(filled.fill(1), std::logic_error);
    CHECK(filled.status() == OrderStatus::Filled);

    Order cancelled = makeOrder(5);
    cancelled.cancel();
    CHECK_THROWS(cancelled.cancel(), std::logic_error);
    CHECK_THROWS(cancelled.fill(1), std::logic_error);
    CHECK_THROWS(cancelled.reject(), std::logic_error);
    CHECK(cancelled.status() == OrderStatus::Cancelled);
    CHECK_EQ(cancelled.remainingQuantity(), 5);

    Order rejected = makeOrder(5);
    rejected.reject();
    CHECK_THROWS(rejected.fill(1), std::logic_error);
    CHECK_THROWS(rejected.cancel(), std::logic_error);
}

TEST(OrderQuantityInvariantAcrossFills) {
    Order o = makeOrder(100);
    for (int i = 0; i < 9; ++i) {
        o.fill(10);
        CHECK(o.remainingQuantity() >= 0);
        CHECK_EQ(o.executedQuantity() + o.remainingQuantity(), o.originalQuantity());
    }
    o.fill(10);
    CHECK(o.status() == OrderStatus::Filled);
    CHECK_EQ(o.executedQuantity(), 100);
}

TEST_MAIN()
