#pragma once

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ExecutionReport.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "OrderTypes.h"
#include "Trade.h"

namespace lob {

// Complete, value-based history of one order. It never refers to anything inside
// the book, so it stays valid however the order leaves it.
//
// Quantities (all in units; executed + remaining + cancelled == effective):
//   submittedQuantity  what the client asked for (never changes)
//   effectiveQuantity  executed + still-working quantity (+ cancelled); equals
//                      submittedQuantity until a modification changes the order
//   executedQuantity   cumulative, only ever grows; modifications never touch it
//   remainingQuantity  still working in the book (0 once filled/cancelled/rejected)
//   cancelledQuantity  what was still working when the order was cancelled
struct OrderRecord {
    OrderId id = 0;
    Side side = Side::Buy;
    Price originalPrice = 0;  // limit price at submission
    Price price = 0;          // current limit price
    Sequence submissionSequence = 0;
    Sequence sequence = 0;    // current time priority (changes when priority is lost)
    Quantity submittedQuantity = 0;
    Quantity effectiveQuantity = 0;
    Quantity executedQuantity = 0;
    Quantity remainingQuantity = 0;
    Quantity cancelledQuantity = 0;
    OrderStatus status = OrderStatus::Rejected;
    RejectReason rejectReason = RejectReason::None;  // why it was rejected
    CancelReason cancelReason = CancelReason::None;  // why it was cancelled
    // Indices into OrderManager::trades() of every trade this order was in, as
    // buyer or seller, strictly increasing (execution order).
    std::vector<std::size_t> tradeIndices;

    bool isActive() const noexcept { return lob::isActive(status); }
};

// The order lifecycle layer above the MatchingEngine: id policy, lifecycle
// history, cancellation, modification and per-order trade history. It talks to
// the book only through the engine's public operations.
//
// ---- Order id policy ------------------------------------------------------
// * Ids are unique for the life of the manager: every id that has ever been
//   recorded is remembered (the `records_` map), whether the order was filled,
//   cancelled or rejected. The OrderBook's resting-id set is NOT the source of
//   truth for this.
// * Ids come from the client (submit(id, ...)) or are generated (submit(...)):
//   a counter starting at `firstGeneratedId` (default 1) that skips ids already
//   used by clients. It never wraps: after max() is issued the generator is
//   exhausted and generated submissions are rejected (IdSpaceExhausted).
// * A submission with a *usable* id (non-zero and unused) is always recorded,
//   even if rejected for its price or quantity, and that id is retired: it
//   appears in history as REJECTED and can never be reused. A submission that
//   has no usable id is rejected without being recorded: id 0
//   (InvalidOrderId), an id already used (DuplicateOrderId, the existing order
//   is untouched), an invalid side (InvalidSide, checked first so no id is
//   burned) or an exhausted generator.
//
// ---- Time priority (the `sequence`) ---------------------------------------
// The manager assigns sequences from one increasing counter: one per accepted
// submission and one per modification that loses priority.
//
// ---- Modification rules (price and/or remaining quantity) -----------------
// `newQuantity` is the new REMAINING quantity; cumulative executed quantity is
// never altered, so effective quantity = executed + newQuantity.
//   price unchanged, remaining decreased  in place, KEEPS time priority
//   price changed (any quantity change)   cancel/replace: LOSES priority, goes to
//                                         the back of the queue at the new price
//                                         and trades first if it now crosses
//   price unchanged, remaining increased  LOSES priority (back of its queue)
//   nothing changed                       rejected as NoChange, order untouched
//   new price <= 0 / new quantity <= 0    rejected (use cancel to remove)
// Only active (NEW / PARTIALLY_FILLED) orders can be modified or cancelled.
//
// ---- Trade history --------------------------------------------------------
// The engine owns the one global trade list. Each record stores the indices of
// its trades. Records are updated only by syncTrades(), which walks the engine's
// trade list from a persistent cursor and applies each trade to its buyer's and
// seller's records exactly once, so no trade can be lost or counted twice, even
// if an operation is interrupted by an exception (the next call resumes it).
//
// ---- Exception safety -----------------------------------------------------
// * Strong (nothing changed): every rejection, an exhausted trade-id space
//   (reported as TradeIdsExhausted and recorded as REJECTED on submit, as a
//   failed MODIFY with the order untouched on modify), cancel, in-place
//   reduction.
// * Allocation failure (std::bad_alloc) in the middle of a submit or a
//   replace is the one case that is only a *basic* guarantee: executions
//   already done stay done and are recorded, the book stays valid, but the
//   order may no longer be resting. The manager reconciles its records before
//   rethrowing: such an order is marked CANCELLED with CancelReason
//   SystemFailure (or FILLED, if its executions completed it), never left
//   "active" without a book entry. This path is not exercised by the tests.
//
// Single-threaded. Memory: history grows without bound (one record per id ever
// used); there is no pruning.
class OrderManager {
public:
    // Throws std::invalid_argument if firstGeneratedId or firstTradeId is 0.
    explicit OrderManager(OrderId firstGeneratedId = 1, TradeId firstTradeId = 1);

    // ---- Operations (each returns an ExecutionReport, see ExecutionReport.h) --

    // Submit with a generated id (reported in the result).
    ExecutionReport submit(Side side, Price price, Quantity quantity);
    // Submit with a client-supplied id.
    ExecutionReport submit(OrderId id, Side side, Price price, Quantity quantity);

    // Cancels the remaining quantity of an active order. Executed quantity and
    // trade history are kept. Rejections: OrderNotFound; OrderNotActive (filled,
    // already cancelled, or rejected).
    ExecutionReport cancel(OrderId id);

    // Modifies price and/or remaining quantity (see the rules above). At least
    // one value is normally supplied; none, or values equal to the current ones,
    // is a NoChange rejection. Rejections: OrderNotFound, OrderNotActive,
    // InvalidPrice, InvalidQuantity, NoChange, IdSpaceExhausted (sequence
    // counter), TradeIdsExhausted.
    ExecutionReport modify(OrderId id, std::optional<Price> newPrice,
                           std::optional<Quantity> newQuantity);

    // ---- Queries ---------------------------------------------------------------

    bool knows(OrderId id) const noexcept { return records_.count(id) != 0; }
    // Snapshot of an order's history (a copy), or nullopt if the id was never recorded.
    std::optional<OrderRecord> order(OrderId id) const;
    // Every trade the order took part in, in execution order (empty if none or unknown).
    std::vector<Trade> tradesFor(OrderId id) const;

    const std::vector<Trade>& trades() const noexcept { return engine_.trades(); }
    const OrderBook& book() const noexcept { return engine_.book(); }
    std::size_t recordedOrderCount() const noexcept { return records_.size(); }
    std::size_t activeOrderCount() const noexcept { return engine_.book().orderCount(); }

    // Full audit of the lifecycle records against the book and the trade list
    // (includes OrderBook::checkInvariants). O(records + trades).
    InvariantReport checkConsistency() const;

private:
    ExecutionReport process(OrderId id, Side side, Price price, Quantity quantity);
    std::optional<OrderId> allocateId();
    bool allocateSequence(Sequence& out);
    void syncTrades();
    void reconcileLostOrder(OrderId id) noexcept;

    MatchingEngine engine_;
    std::unordered_map<OrderId, OrderRecord> records_;
    std::size_t processedTrades_ = 0;

    OrderId nextGeneratedId_;
    bool generatedIdsExhausted_ = false;
    Sequence nextSequence_ = 1;
    bool sequencesExhausted_ = false;
};

}  // namespace lob
