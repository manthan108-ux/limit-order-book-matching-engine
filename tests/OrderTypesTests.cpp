#include "OrderTypes.h"
#include "TestFramework.h"

using namespace lob;

TEST(OppositeSide) {
    CHECK(opposite(Side::Buy) == Side::Sell);
    CHECK(opposite(Side::Sell) == Side::Buy);
}

TEST(StatusToString) {
    CHECK_EQ(toString(OrderStatus::New), "NEW");
    CHECK_EQ(toString(OrderStatus::PartiallyFilled), "PARTIALLY_FILLED");
    CHECK_EQ(toString(OrderStatus::Filled), "FILLED");
    CHECK_EQ(toString(OrderStatus::Cancelled), "CANCELLED");
    CHECK_EQ(toString(OrderStatus::Rejected), "REJECTED");
}

TEST(SideOperationTypeToString) {
    CHECK_EQ(toString(Side::Buy), "BUY");
    CHECK_EQ(toString(Side::Sell), "SELL");
    CHECK_EQ(toString(OrderType::Limit), "LIMIT");
    CHECK_EQ(toString(Operation::Add), "ADD");
    CHECK_EQ(toString(Operation::Cancel), "CANCEL");
    CHECK_EQ(toString(Operation::Modify), "MODIFY");
}

TEST(TerminalAndActiveStatuses) {
    CHECK(isActive(OrderStatus::New));
    CHECK(isActive(OrderStatus::PartiallyFilled));
    CHECK(isTerminal(OrderStatus::Filled));
    CHECK(isTerminal(OrderStatus::Cancelled));
    CHECK(isTerminal(OrderStatus::Rejected));
    CHECK(!isActive(OrderStatus::Filled));
}

TEST(LegalTransitions) {
    CHECK(canTransition(OrderStatus::New, OrderStatus::PartiallyFilled));
    CHECK(canTransition(OrderStatus::New, OrderStatus::Filled));
    CHECK(canTransition(OrderStatus::New, OrderStatus::Cancelled));
    CHECK(canTransition(OrderStatus::New, OrderStatus::Rejected));
    CHECK(canTransition(OrderStatus::PartiallyFilled, OrderStatus::PartiallyFilled));
    CHECK(canTransition(OrderStatus::PartiallyFilled, OrderStatus::Filled));
    CHECK(canTransition(OrderStatus::PartiallyFilled, OrderStatus::Cancelled));
}

TEST(IllegalTransitions) {
    CHECK(!canTransition(OrderStatus::New, OrderStatus::New));
    CHECK(!canTransition(OrderStatus::PartiallyFilled, OrderStatus::New));
    CHECK(!canTransition(OrderStatus::PartiallyFilled, OrderStatus::Rejected));
    // Terminal states never transition anywhere.
    const OrderStatus all[] = {OrderStatus::New, OrderStatus::PartiallyFilled,
                               OrderStatus::Filled, OrderStatus::Cancelled,
                               OrderStatus::Rejected};
    for (OrderStatus to : all) {
        CHECK(!canTransition(OrderStatus::Filled, to));
        CHECK(!canTransition(OrderStatus::Cancelled, to));
        CHECK(!canTransition(OrderStatus::Rejected, to));
    }
}

TEST(PriceAndQuantityValidation) {
    CHECK(isValidPrice(1));
    CHECK(isValidPrice(1000000));
    CHECK(!isValidPrice(0));
    CHECK(!isValidPrice(-100));
    CHECK(isValidQuantity(1));
    CHECK(!isValidQuantity(0));
    CHECK(!isValidQuantity(-5));
}

TEST(RejectReasonMessages) {
    CHECK_EQ(toString(RejectReason::None), "none");
    CHECK(!toString(RejectReason::InvalidOrderId).empty());
    CHECK(!toString(RejectReason::DuplicateOrderId).empty());
    CHECK(!toString(RejectReason::OrderNotFound).empty());
}


TEST(NewReasonsHaveMessages) {
    CHECK(!toString(RejectReason::InvalidSide).empty());
    CHECK(!toString(RejectReason::IdSpaceExhausted).empty());
    CHECK(!toString(RejectReason::TradeIdsExhausted).empty());
    CHECK_EQ(toString(CancelReason::None), "none");
    CHECK(!toString(CancelReason::UserRequested).empty());
    CHECK(!toString(CancelReason::SystemFailure).empty());
}

TEST_MAIN()
