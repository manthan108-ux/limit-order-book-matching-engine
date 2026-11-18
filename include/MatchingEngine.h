#pragma once

#include <optional>
#include <vector>

#include "ExecutionReport.h"
#include "Order.h"
#include "OrderBook.h"
#include "OrderTypes.h"
#include "Trade.h"

namespace lob {

// Price-time-priority matching of incoming limit orders against an OrderBook.
//
// Algorithm for an incoming order (the *taker*), repeated while it has quantity:
//   1. take the highest-priority resting order on the opposite side (the
//      *maker*): best price, then oldest at that price;
//   2. stop if the prices do not cross (buy: taker >= maker; sell: taker <= maker);
//   3. execute min(taker remaining, maker remaining) at the MAKER's price;
//   4. record a Trade, fill the maker through OrderBook::fill (which removes it
//      when complete) and fill the taker.
// Any remainder then rests in the book with the order's original sequence, so
// it queues behind everything already at its price.
//
// Priority is the one OrderBook/PriceLevel already provide (queue position
// within a level, best price across levels); the engine adds no timestamps of
// its own. A trade's `sequence` is the taker's sequence: the arrival event
// that caused it. Trades of one taker share it and are ordered by trade id.
//
// Trade ids: a deterministic counter starting at `firstTradeId` (default 1),
// handed out in execution order. Ids never wrap: if the counter space cannot
// cover the executions an order needs, submit() throws std::overflow_error
// *before* changing anything.
//
// The engine owns its OrderBook and only mutates it through OrderBook's public
// API. Single-threaded; no I/O.
//
// Exception safety: validation rejections and the id-exhaustion check happen
// before any mutation (book untouched). If an allocation fails part-way
// through a match, every execution already applied is complete and recorded
// and the book still satisfies its invariants (basic guarantee), but the
// incoming order is not rested.
class MatchingEngine {
public:
    // Throws std::invalid_argument if firstTradeId == 0 (id 0 is reserved).
    explicit MatchingEngine(TradeId firstTradeId = 1);
    // Starts from an existing book (moved in), e.g. to resume from a snapshot.
    explicit MatchingEngine(OrderBook book, TradeId firstTradeId = 1);

    // Matches and, if anything remains, rests `incoming`.
    // Rejected (book unchanged, report.accepted == false):
    //   DuplicateOrderId  the id is already resting
    //   OrderNotActive    the order is filled / cancelled / rejected
    //   InvalidQuantity   no remaining quantity
    // Throws std::overflow_error if trade ids would run out (nothing changed).
    ExecutionReport submit(Order incoming);

    // Convenience for raw input: validates with Order::validate (invalid id,
    // price or quantity become a rejected report), then submits a LIMIT order.
    ExecutionReport submit(OrderId id, Side side, Price price, Quantity quantity,
                           Sequence sequence);

    // ---- Order maintenance (used by the OrderManager) ----------------------

    // Removes a resting order and returns it marked CANCELLED; nullopt if unknown.
    std::optional<Order> cancel(OrderId id) { return book_.cancel(id); }

    // Lowers a resting order's remaining quantity in place; it keeps its time
    // priority. Throws like OrderBook::reduce. Never trades.
    void reduce(OrderId id, Quantity newRemaining) { book_.reduce(id, newRemaining); }

    // Cancel/replace for changes that lose time priority (a new price, or a
    // larger remaining quantity). The resting order `id` is replaced by a copy
    // with `newPrice`, `newRemaining` and `newSequence`, keeping its id and
    // cumulative executed quantity, and re-enters through normal matching: if the
    // new price crosses the book it trades (at resting prices) before any
    // remainder rests at the back of its new queue. The order is out of the book
    // only inside this call, so two orders with one id never coexist.
    // Returns a report with operation MODIFY. Rejected (book unchanged):
    //   OrderNotFound; InvalidPrice / InvalidQuantity (incl. overflow of
    //   executed + newRemaining). Throws std::overflow_error, with the book
    //   unchanged, if trade ids cannot cover the executions. Strong guarantee for
    //   all of these; see the class comment for allocation failure.
    ExecutionReport replace(OrderId id, Price newPrice, Quantity newRemaining,
                            Sequence newSequence);

    const OrderBook& book() const noexcept { return book_; }
    // Every trade executed so far, in execution order.
    const std::vector<Trade>& trades() const noexcept { return trades_; }

    // Id the next trade will receive; meaningful only if tradeIdsRemaining() > 0.
    TradeId nextTradeId() const noexcept { return nextTradeId_; }
    // How many more trade ids can be issued before exhaustion.
    TradeId tradeIdsRemaining() const noexcept;

private:
    TradeId allocateTradeId();
    // Throws std::overflow_error, changing nothing, if `incoming` could not be
    // numbered in full.
    void ensureTradeIds(const Order& incoming) const;
    // True if resting `quantity` at (side, price) could overflow that level's total quantity.
    // Deliberately conservative: it assumes the whole quantity rests (a remainder can only
    // be smaller). `alreadyCounted` is quantity of the same order that is about to leave
    // that level. Checked BEFORE any mutation, so an order is never half-executed and then
    // unable to rest.
    bool restingWouldOverflow(Side side, Price price, Quantity quantity, Quantity alreadyCounted) const;
    // Match + rest `incoming`, which must not be in the book.
    ExecutionReport execute(Order incoming, Operation operation);

    OrderBook book_;
    std::vector<Trade> trades_;
    TradeId nextTradeId_;
    bool tradeIdsExhausted_ = false;
};

}  // namespace lob
