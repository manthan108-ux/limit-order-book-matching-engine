#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "OrderManager.h"
#include "TestFramework.h"

using namespace lob;

namespace {

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

// Prints any violation so a failure explains itself.
bool ok(const OrderManager& m) {
    const InvariantReport r = m.checkConsistency();
    for (const auto& v : r.violations) std::cerr << "    consistency violated: " << v << "\n";
    return r.ok();
}

std::vector<OrderId> idsAt(const PriceLevel& level) {
    std::vector<OrderId> out;
    for (const Order& o : level) out.push_back(o.id());
    return out;
}

struct Exec {
    OrderId maker;
    OrderId taker;
    Price price;
    Quantity qty;
    bool operator==(const Exec& o) const {
        return maker == o.maker && taker == o.taker && price == o.price && qty == o.qty;
    }
};
std::vector<Exec> execs(const std::vector<Trade>& ts) {
    std::vector<Exec> out;
    for (const Trade& t : ts) out.push_back({t.makerOrderId(), t.takerOrderId(), t.price(), t.quantity()});
    return out;
}

bool notCrossed(const OrderBook& b) {
    auto bid = b.bestBid();
    auto ask = b.bestAsk();
    return !bid || !ask || bid->price < ask->price;
}

}  // namespace

// ---- ID management ---------------------------------------------------------

TEST(GeneratedIdsAreSequentialAndUnique) {
    OrderManager m;
    std::set<OrderId> seen;
    for (OrderId expected = 1; expected <= 20; ++expected) {
        ExecutionReport r = m.submit(expected % 2 ? B : S, 100 + static_cast<Price>(expected), 5);
        CHECK(r.accepted);
        CHECK_EQ(r.orderId, expected);
        CHECK(seen.insert(r.orderId).second);
    }
    CHECK_EQ(m.recordedOrderCount(), 20u);
    CHECK(ok(m));
}

TEST(GeneratedIdsSkipIdsUsedByClients) {
    OrderManager m;
    CHECK_EQ(m.submit(B, 90, 1).orderId, 1u);
    CHECK(m.submit(2, B, 90, 1).accepted);  // client takes id 2
    CHECK(m.submit(3, B, 90, 1).accepted);  // and 3
    CHECK_EQ(m.submit(B, 90, 1).orderId, 4u);  // generator skips both
    CHECK(ok(m));
}

TEST(DuplicateClientIdIsRejectedAndOriginalUntouched) {
    OrderManager m;
    m.submit(7, B, 100, 10);
    ExecutionReport dup = m.submit(7, S, 90, 5);  // would trade if accepted
    CHECK(!dup.accepted);
    CHECK(dup.rejectReason == RejectReason::DuplicateOrderId);
    CHECK_EQ(dup.tradeCount(), 0u);
    CHECK_EQ(m.recordedOrderCount(), 1u);
    CHECK(m.order(7)->side == B);
    CHECK_EQ(m.order(7)->remainingQuantity, 10);
    CHECK_EQ(m.activeOrderCount(), 1u);
    CHECK(ok(m));
}

TEST(IdCannotBeReusedAfterCancel) {
    OrderManager m;
    m.submit(5, B, 100, 10);
    m.cancel(5);
    CHECK(m.book().empty());  // the book alone would happily accept it again
    ExecutionReport r = m.submit(5, B, 100, 10);
    CHECK(!r.accepted);
    CHECK(r.rejectReason == RejectReason::DuplicateOrderId);
    CHECK(m.order(5)->status == OrderStatus::Cancelled);  // history intact
    CHECK(ok(m));
}

TEST(IdCannotBeReusedAfterFullFill) {
    OrderManager m;
    m.submit(5, S, 100, 10);
    m.submit(6, B, 100, 10);
    CHECK(m.order(5)->status == OrderStatus::Filled);
    CHECK(!m.submit(5, S, 100, 10).accepted);
    CHECK(!m.submit(6, B, 100, 10).accepted);
    CHECK(m.book().empty());
    CHECK(ok(m));
}

TEST(IdCannotBeReusedAfterPartialFillThenCancel) {
    OrderManager m;
    m.submit(5, B, 100, 100);
    m.submit(6, S, 100, 40);
    m.cancel(5);
    ExecutionReport r = m.submit(5, B, 100, 100);
    CHECK(r.rejectReason == RejectReason::DuplicateOrderId);
    CHECK_EQ(m.order(5)->executedQuantity, 40);
    CHECK(ok(m));
}

TEST(RejectedSubmissionWithUsableIdIsRecordedAndRetired) {
    OrderManager m;
    ExecutionReport r = m.submit(9, B, 0, 10);  // invalid price
    CHECK(!r.accepted);
    CHECK(r.rejectReason == RejectReason::InvalidPrice);
    CHECK(r.status == OrderStatus::Rejected);
    CHECK(m.knows(9));
    CHECK(m.order(9)->status == OrderStatus::Rejected);
    CHECK(m.order(9)->rejectReason == RejectReason::InvalidPrice);
    CHECK(m.book().empty());
    // The id is retired: it can be neither resubmitted, cancelled nor modified.
    CHECK(m.submit(9, B, 100, 10).rejectReason == RejectReason::DuplicateOrderId);
    CHECK(m.cancel(9).rejectReason == RejectReason::OrderNotActive);
    CHECK(m.modify(9, 100, 5).rejectReason == RejectReason::OrderNotActive);
    CHECK(ok(m));
}

TEST(SubmissionsWithoutUsableIdAreNotRecorded) {
    OrderManager m;
    m.submit(1, B, 100, 10);
    CHECK(m.submit(0, B, 100, 10).rejectReason == RejectReason::InvalidOrderId);
    CHECK(m.submit(1, B, 100, 10).rejectReason == RejectReason::DuplicateOrderId);
    CHECK(m.submit(static_cast<OrderId>(2), static_cast<Side>(9), 100, 10).rejectReason ==
          RejectReason::InvalidSide);
    CHECK(!m.knows(0));
    CHECK(!m.knows(2));  // an invalid side does not burn the id
    CHECK_EQ(m.recordedOrderCount(), 1u);
    CHECK(m.submit(2, B, 100, 10).accepted);
    CHECK(ok(m));
}

TEST(InvalidSideDoesNotConsumeAGeneratedId) {
    OrderManager m;
    CHECK(m.submit(static_cast<Side>(7), 100, 10).rejectReason == RejectReason::InvalidSide);
    CHECK_EQ(m.submit(B, 100, 10).orderId, 1u);
}

TEST(GeneratedIdOverflowNeverWraps) {
    const OrderId max = std::numeric_limits<OrderId>::max();
    OrderManager m(max - 1);
    CHECK_EQ(m.submit(B, 90, 1).orderId, max - 1);
    CHECK_EQ(m.submit(B, 90, 1).orderId, max);
    ExecutionReport r = m.submit(B, 90, 1);  // generator exhausted
    CHECK(!r.accepted);
    CHECK(r.rejectReason == RejectReason::IdSpaceExhausted);
    CHECK_EQ(m.recordedOrderCount(), 2u);  // nothing reused, nothing recorded
    CHECK(!m.submit(B, 90, 1).accepted);   // and it stays exhausted
    CHECK(m.submit(1, B, 90, 1).accepted);  // client ids are unaffected
    CHECK(ok(m));
}

TEST(GeneratorSkipsClientIdsEvenNearTheLimit) {
    const OrderId max = std::numeric_limits<OrderId>::max();
    OrderManager m(max - 1);
    m.submit(max - 1, B, 90, 1);  // client took the first one
    CHECK_EQ(m.submit(B, 90, 1).orderId, max);
    CHECK(!m.submit(B, 90, 1).accepted);
}

TEST(ConstructorRejectsZeroStartingIds) {
    CHECK_THROWS(OrderManager(0), std::invalid_argument);
    CHECK_THROWS(OrderManager(1, 0), std::invalid_argument);
}

// ---- Submission ------------------------------------------------------------------

TEST(ValidSubmissionCreatesHistoryRecord) {
    OrderManager m;
    ExecutionReport r = m.submit(B, 100, 25);
    CHECK(r.accepted);
    CHECK(r.operation == Operation::Add);
    CHECK(r.status == OrderStatus::New);
    CHECK_EQ(r.originalQuantity, 25);
    CHECK_EQ(r.executedQuantity, 0);
    CHECK_EQ(r.remainingQuantity, 25);
    CHECK_EQ(r.tradeCount(), 0u);
    auto rec = m.order(r.orderId);
    CHECK(rec.has_value());
    CHECK(rec->side == B);
    CHECK_EQ(rec->originalPrice, 100);
    CHECK_EQ(rec->price, 100);
    CHECK_EQ(rec->submittedQuantity, 25);
    CHECK_EQ(rec->effectiveQuantity, 25);
    CHECK_EQ(rec->executedQuantity, 0);
    CHECK_EQ(rec->remainingQuantity, 25);
    CHECK_EQ(rec->cancelledQuantity, 0);
    CHECK(rec->status == OrderStatus::New);
    CHECK_EQ(rec->sequence, rec->submissionSequence);
    CHECK(rec->tradeIndices.empty());
    CHECK(m.book().contains(r.orderId));
    CHECK(ok(m));
}

TEST(InvalidPriceAndQuantityAreRejected) {
    OrderManager m;
    CHECK(m.submit(1, B, 0, 10).rejectReason == RejectReason::InvalidPrice);
    CHECK(m.submit(2, B, -5, 10).rejectReason == RejectReason::InvalidPrice);
    CHECK(m.submit(3, B, 100, 0).rejectReason == RejectReason::InvalidQuantity);
    ExecutionReport neg = m.submit(4, B, 100, -3);
    CHECK(neg.rejectReason == RejectReason::InvalidQuantity);
    CHECK_EQ(neg.remainingQuantity, 0);
    CHECK(m.book().empty());
    CHECK(m.trades().empty());
    CHECK_EQ(m.recordedOrderCount(), 4u);
    CHECK(ok(m));
}

TEST(AcceptedOrderWithoutExecutionIsNotARejection) {
    OrderManager m;
    m.submit(1, S, 110, 10);
    ExecutionReport r = m.submit(2, B, 100, 10);
    CHECK(r.accepted);
    CHECK(r.rejectReason == RejectReason::None);
    CHECK(r.status == OrderStatus::New);
    CHECK_EQ(r.tradeCount(), 0u);
    CHECK(ok(m));
}

TEST(FullyMatchedOrderUpdatesBothRecords) {
    OrderManager m;
    m.submit(1, S, 105, 100);
    ExecutionReport r = m.submit(2, B, 110, 100);
    CHECK(r.status == OrderStatus::Filled);
    CHECK((execs(r.trades) == std::vector<Exec>{{1, 2, 105, 100}}));
    CHECK(m.order(1)->status == OrderStatus::Filled);
    CHECK(m.order(2)->status == OrderStatus::Filled);
    CHECK_EQ(m.order(1)->executedQuantity, 100);
    CHECK_EQ(m.order(2)->remainingQuantity, 0);
    CHECK(m.book().empty());
    CHECK_EQ(m.activeOrderCount(), 0u);
    CHECK(ok(m));
}

TEST(PartiallyMatchedOrderRestsTheRemainder) {
    OrderManager m;
    m.submit(1, S, 105, 40);
    ExecutionReport r = m.submit(2, B, 105, 100);
    CHECK(r.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(r.executedQuantity, 40);
    CHECK_EQ(r.remainingQuantity, 60);
    CHECK(m.order(1)->status == OrderStatus::Filled);
    CHECK(m.order(2)->status == OrderStatus::PartiallyFilled);
    CHECK_EQ(m.order(2)->remainingQuantity, 60);
    CHECK(m.book().contains(2));
    CHECK(!m.book().contains(1));
    CHECK(ok(m));
}

TEST(TradeIdExhaustionRejectsAndRetiresTheOrder) {
    const TradeId max = std::numeric_limits<TradeId>::max();
    OrderManager m(1, max);  // exactly one trade id left
    m.submit(1, S, 100, 10);
    m.submit(2, S, 100, 10);
    ExecutionReport r = m.submit(3, B, 100, 20);  // needs two trades
    CHECK(!r.accepted);
    CHECK(r.rejectReason == RejectReason::TradeIdsExhausted);
    CHECK(m.order(3)->status == OrderStatus::Rejected);
    CHECK(!m.book().contains(3));
    CHECK_EQ(m.order(1)->remainingQuantity, 10);  // book untouched
    CHECK(m.trades().empty());
    CHECK(m.submit(3, B, 100, 5).rejectReason == RejectReason::DuplicateOrderId);
    CHECK(m.submit(4, B, 100, 10).accepted);  // one trade fits
    CHECK(ok(m));
}

// ---- Cancellation -----------------------------------------------------------------

TEST(CancelUnfilledOrder) {
    OrderManager m;
    m.submit(1, B, 100, 10);
    ExecutionReport r = m.cancel(1);
    CHECK(r.accepted);
    CHECK(r.operation == Operation::Cancel);
    CHECK(r.status == OrderStatus::Cancelled);
    CHECK(r.cancelReason == CancelReason::UserRequested);
    CHECK_EQ(r.executedQuantity, 0);
    CHECK_EQ(r.remainingQuantity, 0);
    CHECK_EQ(r.originalQuantity, 10);
    CHECK(m.book().empty());
    CHECK_EQ(m.book().bidLevelCount(), 0u);
    CHECK_EQ(m.order(1)->cancelledQuantity, 10);
    CHECK(m.order(1)->cancelReason == CancelReason::UserRequested);
    CHECK(ok(m));
}

TEST(CancelPartiallyFilledOrderKeepsExecutionsAndTrades) {
    OrderManager m;
    m.submit(1, B, 100, 100);
    m.submit(2, S, 100, 40);
    ExecutionReport r = m.cancel(1);
    CHECK(r.accepted);
    CHECK(r.status == OrderStatus::Cancelled);
    CHECK_EQ(r.executedQuantity, 40);
    CHECK_EQ(r.remainingQuantity, 0);
    CHECK_EQ(r.originalQuantity, 100);
    auto rec = m.order(1);
    CHECK_EQ(rec->executedQuantity, 40);
    CHECK_EQ(rec->remainingQuantity, 0);
    CHECK_EQ(rec->cancelledQuantity, 60);
    CHECK_EQ(rec->effectiveQuantity, 100);
    CHECK(!m.book().contains(1));
    CHECK(m.book().empty());
    CHECK_EQ(m.tradesFor(1).size(), 1u);  // the execution is still on record
    CHECK_EQ(m.tradesFor(1)[0].quantity(), 40);
    CHECK(m.order(2)->status == OrderStatus::Filled);  // counterparty unaffected
    CHECK_EQ(m.trades().size(), 1u);
    CHECK(ok(m));
}

TEST(CancelFailures) {
    OrderManager m;
    m.submit(1, S, 100, 10);
    m.submit(2, B, 100, 10);   // 1 and 2 are filled
    m.submit(3, B, 90, 10);
    m.cancel(3);
    m.submit(4, B, 0, 10);     // rejected
    m.submit(5, B, 80, 10);    // untouched bystander

    CHECK(m.cancel(99).rejectReason == RejectReason::OrderNotFound);
    CHECK(!m.cancel(99).accepted);
    ExecutionReport filled = m.cancel(1);
    CHECK(!filled.accepted);
    CHECK(filled.rejectReason == RejectReason::OrderNotActive);
    CHECK(filled.status == OrderStatus::Filled);  // reports its real status
    CHECK_EQ(filled.executedQuantity, 10);
    CHECK(m.cancel(3).rejectReason == RejectReason::OrderNotActive);  // already cancelled
    CHECK(m.cancel(3).rejectReason == RejectReason::OrderNotActive);  // and again
    CHECK(m.cancel(4).rejectReason == RejectReason::OrderNotActive);  // rejected
    CHECK(m.order(1)->status == OrderStatus::Filled);
    CHECK(m.order(3)->status == OrderStatus::Cancelled);
    CHECK(m.book().contains(5));
    CHECK_EQ(m.activeOrderCount(), 1u);
    CHECK(ok(m));
}

TEST(CancelInMiddleKeepsOthersInOrder) {
    OrderManager m;
    m.submit(1, B, 100, 10);
    m.submit(2, B, 100, 10);
    m.submit(3, B, 100, 10);
    m.cancel(2);
    CHECK((idsAt(m.book().bids().at(100)) == std::vector<OrderId>{1, 3}));
    CHECK_EQ(m.book().bestBid()->totalQuantity, 20);
    CHECK(ok(m));
}

// ---- Modification -------------------------------------------------------------------

TEST(ModifyPriceLosesPriorityAndMovesToBackOfNewLevel) {
    OrderManager m;
    m.submit(1, B, 100, 10);
    m.submit(2, B, 101, 10);
    m.submit(3, B, 101, 10);
    const Sequence before = m.order(1)->sequence;
    ExecutionReport r = m.modify(1, 101, std::nullopt);
    CHECK(r.accepted);
    CHECK(r.operation == Operation::Modify);
    CHECK(r.status == OrderStatus::New);
    CHECK_EQ(r.remainingQuantity, 10);
    CHECK((idsAt(m.book().bids().at(101)) == std::vector<OrderId>{2, 3, 1}));
    CHECK(m.book().bids().count(100) == 0);
    CHECK_EQ(m.order(1)->price, 101);
    CHECK_EQ(m.order(1)->originalPrice, 100);  // history keeps the first price
    CHECK(m.order(1)->sequence > before);
    CHECK_EQ(m.order(1)->submissionSequence, before);
    CHECK_EQ(m.book().orderCount(), 3u);       // never two copies
    CHECK(ok(m));
}

TEST(ModifyQuantityIncreaseLosesPriority) {
    OrderManager m;
    m.submit(1, B, 100, 10);
    m.submit(2, B, 100, 10);
    m.submit(3, B, 100, 10);
    ExecutionReport r = m.modify(1, std::nullopt, 25);
    CHECK(r.accepted);
    CHECK((idsAt(m.book().bids().at(100)) == std::vector<OrderId>{2, 3, 1}));
    CHECK_EQ(m.book().find(1)->remainingQuantity(), 25);
    CHECK_EQ(m.book().bestBid()->totalQuantity, 45);
    CHECK_EQ(m.order(1)->effectiveQuantity, 25);
    CHECK_EQ(m.order(1)->submittedQuantity, 10);  // history keeps what was asked
    CHECK(ok(m));
}

TEST(ModifyQuantityDecreaseKeepsPriority) {
    OrderManager m;
    m.submit(1, B, 100, 10);
    m.submit(2, B, 100, 10);
    m.submit(3, B, 100, 10);
    const Sequence before = m.order(1)->sequence;
    ExecutionReport r = m.modify(1, std::nullopt, 4);
    CHECK(r.accepted);
    CHECK_EQ(r.remainingQuantity, 4);
    CHECK((idsAt(m.book().bids().at(100)) == std::vector<OrderId>{1, 2, 3}));
    CHECK_EQ(m.order(1)->sequence, before);
    CHECK_EQ(m.book().bestBid()->totalQuantity, 24);
    CHECK_EQ(m.book().find(1)->remainingQuantity(), 4);
    CHECK(ok(m));
}

TEST(ModifyPriceWithQuantityDecreaseStillLosesPriority) {
    OrderManager m;
    m.submit(1, B, 100, 10);
    m.submit(2, B, 101, 10);
    m.modify(1, 101, 3);
    CHECK((idsAt(m.book().bids().at(101)) == std::vector<OrderId>{2, 1}));
    CHECK_EQ(m.book().find(1)->remainingQuantity(), 3);
    CHECK(ok(m));
}

TEST(ModifyWithNoChangeIsRejectedAndLeavesOrderAlone) {
    OrderManager m;
    m.submit(1, B, 100, 10);
    m.submit(2, B, 100, 10);
    const Sequence before = m.order(1)->sequence;
    CHECK(m.modify(1, 100, 10).rejectReason == RejectReason::NoChange);
    CHECK(m.modify(1, 100, std::nullopt).rejectReason == RejectReason::NoChange);
    CHECK(m.modify(1, std::nullopt, 10).rejectReason == RejectReason::NoChange);
    CHECK(m.modify(1, std::nullopt, std::nullopt).rejectReason == RejectReason::NoChange);
    CHECK(!m.modify(1, 100, 10).accepted);
    CHECK((idsAt(m.book().bids().at(100)) == std::vector<OrderId>{1, 2}));
    CHECK_EQ(m.order(1)->sequence, before);
    CHECK(ok(m));
}

TEST(ModifyRejectsInvalidValuesWithoutChange) {
    OrderManager m;
    m.submit(1, B, 100, 10);
    CHECK(m.modify(1, 0, std::nullopt).rejectReason == RejectReason::InvalidPrice);
    CHECK(m.modify(1, -4, 5).rejectReason == RejectReason::InvalidPrice);
    CHECK(m.modify(1, std::nullopt, 0).rejectReason == RejectReason::InvalidQuantity);
    CHECK(m.modify(1, 101, -2).rejectReason == RejectReason::InvalidQuantity);
    CHECK_EQ(m.book().find(1)->price(), 100);
    CHECK_EQ(m.book().find(1)->remainingQuantity(), 10);
    CHECK(ok(m));
}

TEST(ModifyUnknownCancelledFilledAndRejectedOrders) {
    OrderManager m;
    m.submit(1, S, 100, 10);
    m.submit(2, B, 100, 10);  // 1, 2 filled
    m.submit(3, B, 90, 10);
    m.cancel(3);
    m.submit(4, B, 0, 10);    // rejected
    CHECK(m.modify(99, 100, 5).rejectReason == RejectReason::OrderNotFound);
    CHECK(m.modify(1, 101, 5).rejectReason == RejectReason::OrderNotActive);
    CHECK(m.modify(3, 91, 5).rejectReason == RejectReason::OrderNotActive);
    CHECK(m.modify(4, 91, 5).rejectReason == RejectReason::OrderNotActive);
    CHECK(m.modify(1, 101, 5).status == OrderStatus::Filled);
    CHECK(m.book().empty());
    CHECK(ok(m));
}

TEST(ModifyPreservesCumulativeExecutedQuantity) {
    OrderManager m;
    m.submit(1, B, 100, 100);
    m.submit(9, S, 100, 40);  // executes 40 of order 1
    m.submit(2, B, 100, 10);  // arrives later at the same price
    CHECK_EQ(m.order(1)->executedQuantity, 40);
    CHECK_EQ(m.order(1)->remainingQuantity, 60);

    // Reduce remaining to 30: executed stays 40, effective total becomes 70, keeps priority.
    ExecutionReport down = m.modify(1, std::nullopt, 30);
    CHECK(down.accepted);
    CHECK(down.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(down.executedQuantity, 40);
    CHECK_EQ(down.remainingQuantity, 30);
    CHECK_EQ(down.originalQuantity, 70);
    CHECK_EQ(m.order(1)->effectiveQuantity, 70);
    CHECK_EQ(m.order(1)->submittedQuantity, 100);
    CHECK_EQ(m.book().find(1)->originalQuantity(), 70);
    CHECK_EQ(m.book().find(1)->executedQuantity(), 40);
    CHECK((idsAt(m.book().bids().at(100)) == std::vector<OrderId>{1, 2}));

    // Raise remaining to 80: executed stays 40, effective total 120, goes to the back.
    ExecutionReport up = m.modify(1, std::nullopt, 80);
    CHECK(up.accepted);
    CHECK(up.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(up.executedQuantity, 40);
    CHECK_EQ(up.remainingQuantity, 80);
    CHECK_EQ(up.originalQuantity, 120);
    CHECK_EQ(m.order(1)->effectiveQuantity, 120);
    CHECK_EQ(m.book().find(1)->originalQuantity(), 120);
    CHECK((idsAt(m.book().bids().at(100)) == std::vector<OrderId>{2, 1}));
    CHECK_EQ(m.tradesFor(1).size(), 1u);  // history is untouched by modifications
    CHECK(ok(m));
}

TEST(ModifyPriceIntoTheBookTradesAtRestingPrices) {
    OrderManager m;
    m.submit(1, S, 105, 10);
    m.submit(2, S, 106, 10);
    m.submit(3, B, 100, 15);
    ExecutionReport r = m.modify(3, 106, std::nullopt);
    CHECK(r.accepted);
    CHECK((execs(r.trades) == std::vector<Exec>{{1, 3, 105, 10}, {2, 3, 106, 5}}));
    CHECK(r.status == OrderStatus::Filled);
    CHECK_EQ(r.executedQuantity, 15);
    CHECK(m.order(3)->status == OrderStatus::Filled);
    CHECK_EQ(m.order(3)->tradeIndices.size(), 2u);
    CHECK(m.order(1)->status == OrderStatus::Filled);
    CHECK_EQ(m.order(2)->remainingQuantity, 5);
    CHECK(!m.book().contains(3));
    CHECK(ok(m));
}

TEST(ModifyCrossingPartiallyThenRestsAtNewPrice) {
    OrderManager m;
    m.submit(1, S, 105, 4);
    m.submit(2, B, 100, 10);
    ExecutionReport r = m.modify(2, 105, std::nullopt);
    CHECK_EQ(r.tradeCount(), 1u);
    CHECK(r.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(r.remainingQuantity, 6);
    CHECK_EQ(m.book().find(2)->price(), 105);
    CHECK_EQ(m.book().find(2)->remainingQuantity(), 6);
    CHECK(notCrossed(m.book()));
    CHECK(ok(m));
}

TEST(ModifiedOrderIsServedAccordingToItsNewPriority) {
    OrderManager m;
    m.submit(1, S, 100, 10);
    m.submit(2, S, 100, 10);
    m.submit(3, S, 100, 10);
    m.modify(1, std::nullopt, 20);  // increase: loses priority, queue is 2, 3, 1
    ExecutionReport r = m.submit(4, B, 100, 10);
    CHECK((execs(r.trades) == std::vector<Exec>{{2, 4, 100, 10}}));

    OrderManager k;
    k.submit(1, S, 100, 10);
    k.submit(2, S, 100, 10);
    k.modify(2, std::nullopt, 5);   // decrease: queue unchanged, 1 then 2
    ExecutionReport r2 = k.submit(3, B, 100, 12);
    CHECK((execs(r2.trades) == std::vector<Exec>{{1, 3, 100, 10}, {2, 3, 100, 2}}));
    CHECK(ok(m));
    CHECK(ok(k));
}

TEST(ModifyWithExhaustedTradeIdsLeavesOrderUntouched) {
    const TradeId max = std::numeric_limits<TradeId>::max();
    OrderManager m(1, max);  // one trade id left
    m.submit(1, S, 105, 10);
    m.submit(2, S, 105, 10);
    m.submit(3, B, 100, 20);
    m.submit(4, B, 100, 5);
    const Sequence before = m.order(3)->sequence;
    ExecutionReport r = m.modify(3, 105, std::nullopt);  // would need two trades
    CHECK(!r.accepted);
    CHECK(r.rejectReason == RejectReason::TradeIdsExhausted);
    CHECK_EQ(m.book().find(3)->price(), 100);              // still resting, unchanged
    CHECK((idsAt(m.book().bids().at(100)) == std::vector<OrderId>{3, 4}));  // same place
    CHECK_EQ(m.order(3)->sequence, before);
    CHECK(m.trades().empty());
    CHECK(ok(m));
}

TEST(ModifyQuantityOverflowIsRejected) {
    OrderManager m;
    m.submit(1, B, 100, 100);
    m.submit(2, S, 100, 40);  // executed 40
    ExecutionReport r = m.modify(1, std::nullopt, std::numeric_limits<Quantity>::max());
    CHECK(!r.accepted);
    CHECK(r.rejectReason == RejectReason::InvalidQuantity);
    CHECK_EQ(m.book().find(1)->remainingQuantity(), 60);
    CHECK(ok(m));
}

// ---- Trade history ---------------------------------------------------------------------

TEST(TradeHistoryForBuyerAndSeller) {
    OrderManager m;
    m.submit(1, S, 100, 10);
    m.submit(2, S, 101, 10);
    m.submit(3, B, 101, 15);
    auto buyer = m.tradesFor(3);
    auto sellerA = m.tradesFor(1);
    auto sellerB = m.tradesFor(2);
    CHECK_EQ(buyer.size(), 2u);
    CHECK_EQ(sellerA.size(), 1u);
    CHECK_EQ(sellerB.size(), 1u);
    CHECK_EQ(buyer[0].buyOrderId(), 3u);
    CHECK_EQ(buyer[0].sellOrderId(), 1u);
    CHECK_EQ(buyer[1].sellOrderId(), 2u);
    CHECK_EQ(sellerA[0].id(), buyer[0].id());  // the same trade from both sides
    CHECK_EQ(sellerB[0].quantity(), 5);
    CHECK(m.tradesFor(999).empty());
    CHECK(ok(m));
}

TEST(TradeHistorySurvivesFullFillPartialFillAndCancellation) {
    OrderManager m;
    m.submit(1, B, 100, 30);  // will be partially filled twice, then cancelled
    m.submit(2, S, 100, 10);  // executes 10 of order 1
    m.submit(4, S, 100, 15);  // executes 15 more: order 1 has 5 left
    m.submit(3, S, 110, 5);   // separate pair on the ask side, far from the bids
    m.submit(5, B, 110, 5);   // fully fills order 3
    CHECK(m.order(3)->status == OrderStatus::Filled);
    CHECK_EQ(m.tradesFor(3).size(), 1u);  // history after a full fill
    CHECK(m.order(1)->status == OrderStatus::PartiallyFilled);
    CHECK_EQ(m.tradesFor(1).size(), 2u);  // history after a partial fill
    m.cancel(1);
    CHECK(m.order(1)->status == OrderStatus::Cancelled);
    CHECK_EQ(m.tradesFor(1).size(), 2u);  // and after cancellation
    CHECK_EQ(m.tradesFor(1)[0].quantity(), 10);
    CHECK_EQ(m.tradesFor(1)[1].quantity(), 15);
    CHECK_EQ(m.order(1)->cancelledQuantity, 5);
    CHECK_EQ(m.tradesFor(4).size(), 1u);
    CHECK_EQ(m.tradesFor(2).size(), 1u);
    CHECK_EQ(m.tradesFor(5).size(), 1u);
    CHECK(ok(m));
}

TEST(TradeHistoryHasNoDuplicatesAndIsChronological) {
    OrderManager m;
    for (OrderId i = 1; i <= 6; ++i) m.submit(i, S, 100 + static_cast<Price>(i % 3), 7);
    m.submit(10, B, 110, 20);
    m.submit(11, B, 110, 20);
    m.modify(10, 111, std::nullopt);  // fully filled already: rejected, no new history
    for (OrderId id = 1; id <= 11; ++id) {
        const auto ts = m.tradesFor(id);
        std::set<TradeId> unique;
        TradeId last = 0;
        for (const Trade& t : ts) {
            CHECK(unique.insert(t.id()).second);  // no duplicate association
            CHECK(t.id() > last);                 // chronological
            last = t.id();
            CHECK(t.buyOrderId() == id || t.sellOrderId() == id);
        }
    }
    // Every global trade is on exactly its buyer and its seller.
    for (const Trade& t : m.trades()) {
        auto onBuyer = m.tradesFor(t.buyOrderId());
        auto onSeller = m.tradesFor(t.sellOrderId());
        CHECK_EQ(std::count_if(onBuyer.begin(), onBuyer.end(),
                               [&t](const Trade& x) { return x.id() == t.id(); }), 1);
        CHECK_EQ(std::count_if(onSeller.begin(), onSeller.end(),
                               [&t](const Trade& x) { return x.id() == t.id(); }), 1);
    }
    CHECK(ok(m));
}

// ---- Reference-model lifecycle stress ----------------------------------------------------

namespace {

struct MOrder {
    OrderId id = 0;
    Side side = B;
    Price price = 0;
    Quantity effective = 0, executed = 0, remaining = 0, cancelled = 0;
    OrderStatus status = OrderStatus::New;
    RejectReason rejected = RejectReason::None;
    std::uint64_t stamp = 0;  // the model's own notion of time priority
    std::vector<Exec> trades;
    bool active() const { return status == OrderStatus::New || status == OrderStatus::PartiallyFilled; }
};

// Naive independent model of the whole lifecycle. Shares no code with the manager.
struct Model {
    std::map<OrderId, MOrder> all;
    std::uint64_t clock = 0;

    static bool better(Side s, Price a, Price b) { return s == B ? a > b : a < b; }

    std::vector<Exec> match(MOrder& in) {
        std::vector<Exec> out;
        const Side opp = in.side == B ? S : B;
        while (in.remaining > 0) {
            MOrder* best = nullptr;
            for (auto& kv : all) {
                MOrder& o = kv.second;
                if (!o.active() || o.side != opp || o.id == in.id) continue;
                const bool crosses = in.side == B ? in.price >= o.price : in.price <= o.price;
                if (!crosses) continue;
                if (best == nullptr || better(opp, o.price, best->price) ||
                    (o.price == best->price && o.stamp < best->stamp)) {
                    best = &o;
                }
            }
            if (best == nullptr) break;
            const Quantity q = std::min(in.remaining, best->remaining);
            const Exec e{best->id, in.id, best->price, q};
            for (MOrder* x : {&in, best}) {
                x->executed += q;
                x->remaining -= q;
                x->status = x->remaining == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
                x->trades.push_back(e);
            }
            out.push_back(e);
        }
        return out;
    }

    std::vector<Exec> submit(OrderId id, Side side, Price price, Quantity qty) {
        MOrder o;
        o.id = id; o.side = side; o.price = price; o.effective = qty;
        if (price <= 0 || qty <= 0) {
            o.status = OrderStatus::Rejected;
            o.rejected = price <= 0 ? RejectReason::InvalidPrice : RejectReason::InvalidQuantity;
            all[id] = o;
            return {};
        }
        o.remaining = qty;
        o.stamp = ++clock;
        all[id] = o;
        return match(all[id]);
    }

    RejectReason cancel(OrderId id) {
        auto it = all.find(id);
        if (it == all.end()) return RejectReason::OrderNotFound;
        MOrder& o = it->second;
        if (!o.active()) return RejectReason::OrderNotActive;
        o.cancelled = o.remaining;
        o.remaining = 0;
        o.status = OrderStatus::Cancelled;
        return RejectReason::None;
    }

    // Returns the failure reason (None on success); executions go to `out`.
    RejectReason modify(OrderId id, std::optional<Price> p, std::optional<Quantity> q,
                        std::vector<Exec>& out) {
        auto it = all.find(id);
        if (it == all.end()) return RejectReason::OrderNotFound;
        MOrder& o = it->second;
        if (!o.active()) return RejectReason::OrderNotActive;
        if (p && *p <= 0) return RejectReason::InvalidPrice;
        if (q && *q <= 0) return RejectReason::InvalidQuantity;
        const Price np = p.value_or(o.price);
        const Quantity nq = q.value_or(o.remaining);
        if (np == o.price && nq == o.remaining) return RejectReason::NoChange;
        if (np == o.price && nq < o.remaining) {  // in place, priority kept
            o.remaining = nq;
            o.effective = o.executed + nq;
            return RejectReason::None;
        }
        o.price = np;                              // priority lost
        o.remaining = nq;
        o.effective = o.executed + nq;
        o.stamp = ++clock;
        out = match(o);
        return RejectReason::None;
    }
};

void compareRecord(const OrderManager& m, const MOrder& o) {
    auto rec = m.order(o.id);
    CHECK(rec.has_value());
    if (!rec) return;
    CHECK(rec->status == o.status);
    CHECK_EQ(rec->executedQuantity, o.executed);
    CHECK_EQ(rec->remainingQuantity, o.remaining);
    CHECK_EQ(rec->cancelledQuantity, o.cancelled);
    CHECK_EQ(rec->effectiveQuantity, o.effective);
    CHECK_EQ(rec->price, o.price);
    CHECK(rec->rejectReason == o.rejected);
    CHECK((execs(m.tradesFor(o.id)) == o.trades));
    CHECK_EQ(m.book().contains(o.id), o.active());
}

void runLifecycleStress(std::uint64_t seed, int steps) {
    std::uint64_t state = seed;
    auto next = [&state](std::uint64_t bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return (state >> 33) % bound;
    };

    OrderManager m;
    Model model;
    std::vector<OrderId> known;  // every id the model has seen
    OrderId nextGenerated = 1;

    for (int step = 0; step < steps; ++step) {
        const std::uint64_t pick = next(100);
        if (pick < 45) {  // ---- submit
            const Side side = next(2) == 0 ? B : S;
            Price price = 95 + static_cast<Price>(next(11));
            Quantity qty = 1 + static_cast<Quantity>(next(40));
            if (next(100) < 5) { if (next(2) == 0) price = 0; else qty = 0; }  // invalid input
            const bool generated = next(100) < 70;
            OrderId id;
            ExecutionReport r;
            if (generated) {
                id = nextGenerated;
                r = m.submit(side, price, qty);
                CHECK_EQ(r.orderId, id);
                ++nextGenerated;
                while (model.all.count(nextGenerated) != 0) ++nextGenerated;  // skip client ids
            } else {
                id = 100000 + static_cast<OrderId>(step);
                r = m.submit(id, side, price, qty);
            }
            if (generated && model.all.count(id) != 0) { CHECK(false); continue; }
            const std::vector<Exec> expected = model.submit(id, side, price, qty);
            known.push_back(id);
            const MOrder& o = model.all.at(id);
            CHECK_EQ(r.accepted, o.status != OrderStatus::Rejected);
            CHECK(r.status == o.status);
            CHECK((execs(r.trades) == expected));
            CHECK_EQ(r.executedQuantity, o.executed);
            CHECK_EQ(r.remainingQuantity, o.remaining);
            compareRecord(m, o);
        } else if (pick < 65) {  // ---- cancel (known id, or occasionally an unknown one)
            const OrderId id = known.empty() || next(10) == 0
                                   ? 9999999
                                   : known[static_cast<std::size_t>(next(known.size()))];
            const ExecutionReport r = m.cancel(id);
            const RejectReason want = model.cancel(id);
            CHECK_EQ(r.accepted, want == RejectReason::None);
            CHECK(r.rejectReason == want);
            if (model.all.count(id) != 0) {
                CHECK(r.status == model.all.at(id).status);
                compareRecord(m, model.all.at(id));
            }
        } else {  // ---- modify
            const OrderId id = known.empty() || next(10) == 0
                                   ? 9999999
                                   : known[static_cast<std::size_t>(next(known.size()))];
            std::optional<Price> p;
            std::optional<Quantity> q;
            if (next(2) == 0) p = 95 + static_cast<Price>(next(11));
            if (next(2) == 0) q = 1 + static_cast<Quantity>(next(40));
            if (next(40) == 0) p = 0;
            if (next(40) == 0) q = 0;
            std::vector<Exec> expected;
            const RejectReason want = model.modify(id, p, q, expected);
            const ExecutionReport r = m.modify(id, p, q);
            CHECK_EQ(r.accepted, want == RejectReason::None);
            CHECK(r.rejectReason == want);
            CHECK((execs(r.trades) == expected));
            if (model.all.count(id) != 0) {
                const MOrder& o = model.all.at(id);
                CHECK(r.status == o.status);
                CHECK_EQ(r.executedQuantity, o.executed);
                CHECK_EQ(r.remainingQuantity, o.remaining);
                compareRecord(m, o);
                // A trade changes the counterparties' records too: spot-check them.
                for (const Exec& e : expected) compareRecord(m, model.all.at(e.maker));
            }
        }

        CHECK(ok(m));
        CHECK(notCrossed(m.book()));
        std::size_t activeInModel = 0;
        for (const auto& kv : model.all) if (kv.second.active()) ++activeInModel;
        CHECK_EQ(m.activeOrderCount(), activeInModel);
        CHECK_EQ(m.recordedOrderCount(), model.all.size());
    }

    // Full comparison of every record and its trade history at the end.
    std::size_t modelTrades = 0;
    for (const auto& kv : model.all) {
        compareRecord(m, kv.second);
        modelTrades += kv.second.trades.size();
    }
    CHECK_EQ(modelTrades, m.trades().size() * 2);  // each trade is on exactly two orders
    TradeId expectedId = 1;
    for (const Trade& t : m.trades()) CHECK_EQ(t.id(), expectedId++);
}

}  // namespace

TEST(LifecycleStressAgainstReferenceModelSeedA) { runLifecycleStress(424242, 3000); }
TEST(LifecycleStressAgainstReferenceModelSeedB) { runLifecycleStress(8675309, 3000); }
TEST(LifecycleStressAgainstReferenceModelSeedC) { runLifecycleStress(1357913, 3000); }

TEST_MAIN()
