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

// The one violation checkConsistency() reports when recovery or trade
// synchronization is pending (see OrderManager::recover). It is not corruption.
inline constexpr const char* kHistoryNotCurrent =
    "history is not current: recovery or trade synchronization is pending (call recover())";

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
// ---- Exception safety (verified by tests/FaultInjectionTests.cpp) ---------
// Three kinds of outcome, per operation:
//  1. A rejected business operation: a normal report, nothing thrown.
//  2. STRONG guarantee (an exception leaves everything as it was):
//     - every operation, when the failure happens before the engine has
//       executed anything: record creation; a submit that dies before its
//       first execution (its record is rolled back, so the id stays free);
//     - cancel and in-place quantity reduction never allocate at all;
//     - the queries (order, tradesFor) never change state.
//  3. BASIC guarantee (valid state, but not "as if nothing happened"): a
//     std::bad_alloc after a submit or cancel/replace has ALREADY executed
//     trades, or after a cancel/replace has taken the order out of the book.
//     Executions done stay done and are recorded exactly once; the book keeps
//     all its invariants; the order that could not be rested is marked
//     CANCELLED with CancelReason::SystemFailure (cancelledQuantity = what was
//     left), or FILLED if its executions completed it. A cancel/replace that
//     fails before the order leaves the book leaves it untouched; one that
//     fails after leaves it cancelled (it is NOT restored). The exception is
//     rethrown, so the caller learns the outcome from order(id).
//
// Bookkeeping that follows a successful engine operation (applying its trades
// to the records) can itself fail to allocate. That does not turn a completed
// operation into an exception: the report is returned (it comes from the
// engine), and the records catch up at the start of the next mutating call, or
// on an explicit recover(). Likewise, if recovery after a failure is itself
// starved of memory, a small "in-flight" marker (no allocation) remembers the
// unfinished order and the next call completes the recovery before doing
// anything else. Until then isHistoryCurrent() is false and snapshots may lag;
// they are never inconsistent with themselves (checkConsistency says so).
// Trade ids consumed by a failed attempt are not reused, so ids may have gaps
// after a failure (they are still unique and increasing).
//
// ---- API contracts kept stable ---------------------------------------------
// * order() returns a value copy; tradesFor() returns copies.
// * modify with no change -> rejected as NoChange; remaining quantity 0 ->
//   rejected (use cancel). Quantity decrease keeps FIFO priority; a price
//   change or quantity increase loses it.
// * A recorded id (filled, cancelled or rejected) is never reused.
//
// ---- History retention -------------------------------------------------------
// Every OrderRecord and every trade is kept for the manager's lifetime, and
// every recorded id stays reserved for that lifetime. Memory grows with the
// number of recorded orders and trades; there is no pruning API. A future
// retention or persistence policy must keep id uniqueness (retired ids must
// stay unavailable) and the semantics of order() / tradesFor() for old orders.
//
// Single-threaded.
class OrderManager {
public:
    // Throws std::invalid_argument if firstGeneratedId or firstTradeId is 0.
    explicit OrderManager(OrderId firstGeneratedId = 1, TradeId firstTradeId = 1);

    // ---- Operations (each returns an ExecutionReport, see ExecutionReport.h) --

    // Rejections of a submit (either form), see the id policy above for which are
    // recorded: InvalidSide, InvalidOrderId, DuplicateOrderId, IdSpaceExhausted (no
    // generated id or sequence number left), InvalidPrice, InvalidQuantity,
    // TradeIdsExhausted, QuantityOverflow (resting the quantity would overflow the
    // total of its price level).
    //
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
    // counter), TradeIdsExhausted, QuantityOverflow (the order would overflow the
    // total quantity of the price level it rests at; the order is left untouched).
    ExecutionReport modify(OrderId id, std::optional<Price> newPrice,
                           std::optional<Quantity> newQuantity);

    // ---- Recovery ----------------------------------------------------------------

    // Completes any bookkeeping an earlier std::bad_alloc left unfinished.
    // Safe to call at any time, idempotent, never throws. Returns true when the
    // history is current afterwards (false if memory is still exhausted).
    // Every mutating operation does this first anyway.
    bool recover() noexcept;
    // True when no recovery is pending and every engine trade has been applied
    // to the records. Queries are exact only while this holds.
    bool isHistoryCurrent() const noexcept {
        return inFlight_.id == 0 && processedTrades_ == engine_.trades().size();
    }

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
    // Applies not-yet-applied engine trades to the records, one trade at a time
    // and each completely or not at all. Throws only std::bad_alloc (retryable)
    // or std::logic_error (a bug).
    void syncTrades();
    // Same, after a successful engine operation: allocation failure is deferred.
    void syncTradesDeferringAllocationFailure();
    // Finishes the operation recorded in inFlight_, if any (see below).
    void recoverInFlight();
    // recoverInFlight() then syncTrades(): run at the start of mutating calls.
    void prepare();

    // The one submit or cancel/replace whose engine call has started but whose
    // outcome has not yet been reconciled with the records. Plain data, so
    // setting and clearing it cannot fail.
    struct InFlight {
        OrderId id = 0;                 // 0 = none
        std::size_t tradesBefore = 0;   // engine trade count when it started
        bool isReplace = false;         // cancel/replace (else a submit)
        bool termsApplied = false;      // replacement terms already put on the record
        Price price = 0;                // replacement terms
        Sequence sequence = 0;
        Quantity quantity = 0;
    };

    MatchingEngine engine_;
    std::unordered_map<OrderId, OrderRecord> records_;
    std::size_t processedTrades_ = 0;
    InFlight inFlight_;

    OrderId nextGeneratedId_;
    bool generatedIdsExhausted_ = false;
    Sequence nextSequence_ = 1;
    bool sequencesExhausted_ = false;
};

}  // namespace lob
