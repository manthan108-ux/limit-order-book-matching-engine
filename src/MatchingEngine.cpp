#include "MatchingEngine.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace lob {

namespace {

// Does an incoming order at `incomingPrice` trade against a resting `restingPrice`?
bool crosses(Side incomingSide, Price incomingPrice, Price restingPrice) noexcept {
    return incomingSide == Side::Buy ? incomingPrice >= restingPrice
                                     : incomingPrice <= restingPrice;
}

// Read-only dry run: how many executions `incoming` would need against `opposite`
// (one per resting order it touches). Used only when trade ids are scarce, to
// decide up front whether the whole match can be numbered.
template <typename Map>
std::uint64_t countExecutions(const Map& opposite, const Order& incoming) {
    Quantity remaining = incoming.remainingQuantity();
    std::uint64_t executions = 0;
    for (const auto& levelEntry : opposite) {
        if (!crosses(incoming.side(), incoming.price(), levelEntry.first)) break;
        for (const Order& resting : levelEntry.second) {
            ++executions;
            if (resting.remainingQuantity() >= remaining) return executions;
            remaining -= resting.remainingQuantity();
        }
    }
    return executions;
}

ExecutionReport rejection(OrderId id, Quantity originalQuantity, RejectReason reason) {
    ExecutionReport r;
    r.operation = Operation::Add;
    r.orderId = id;
    r.accepted = false;
    r.rejectReason = reason;
    r.status = OrderStatus::Rejected;
    r.originalQuantity = originalQuantity;
    return r;
}

}  // namespace

MatchingEngine::MatchingEngine(TradeId firstTradeId) : MatchingEngine(OrderBook(), firstTradeId) {}

MatchingEngine::MatchingEngine(OrderBook book, TradeId firstTradeId)
    : book_(std::move(book)), trades_(), nextTradeId_(firstTradeId) {
    if (firstTradeId == 0) {
        throw std::invalid_argument("first trade id must be > 0 (0 is reserved)");
    }
}

TradeId MatchingEngine::tradeIdsRemaining() const noexcept {
    if (tradeIdsExhausted_) return 0;
    // nextTradeId_ >= 1, so this cannot overflow; at nextTradeId_ == max it is 1.
    return std::numeric_limits<TradeId>::max() - nextTradeId_ + 1;
}

TradeId MatchingEngine::allocateTradeId() {
    if (tradeIdsExhausted_) throw std::overflow_error("trade id space exhausted");
    const TradeId id = nextTradeId_;
    if (id == std::numeric_limits<TradeId>::max()) {
        tradeIdsExhausted_ = true;  // never wrap around and reuse ids
    } else {
        ++nextTradeId_;
    }
    return id;
}

ExecutionReport MatchingEngine::submit(OrderId id, Side side, Price price,
                                       Quantity quantity, Sequence sequence) {
    const RejectReason invalid = Order::validate(id, price, quantity);
    if (invalid != RejectReason::None) return rejection(id, quantity, invalid);
    return submit(*Order::create(id, side, OrderType::Limit, price, quantity, sequence));
}

ExecutionReport MatchingEngine::submit(Order incoming) {
    const OrderId id = incoming.id();
    const Quantity original = incoming.originalQuantity();

    if (book_.contains(id)) return rejection(id, original, RejectReason::DuplicateOrderId);
    if (!incoming.isActive()) return rejection(id, original, RejectReason::OrderNotActive);
    if (incoming.remainingQuantity() <= 0) {
        return rejection(id, original, RejectReason::InvalidQuantity);
    }

    const Side side = incoming.side();
    const Side opposite = lob::opposite(side);

    // Fail before any mutation if the executions could not all get a trade id.
    // Fast path: enough ids for even one trade per resting order. Otherwise
    // count exactly, since ids this scarce are an extreme corner case.
    if (tradeIdsRemaining() < book_.orderCount()) {
        const std::uint64_t needed = side == Side::Buy
                                         ? countExecutions(book_.asks(), incoming)
                                         : countExecutions(book_.bids(), incoming);
        if (needed > tradeIdsRemaining()) {
            throw std::overflow_error("trade id space cannot cover this order's executions");
        }
    }

    ExecutionReport report;
    report.operation = Operation::Add;
    report.orderId = id;
    report.accepted = true;
    report.originalQuantity = original;

    while (incoming.remainingQuantity() > 0) {
        const Order* best = book_.bestOrder(opposite);
        if (best == nullptr || !crosses(side, incoming.price(), best->price())) break;

        // Copy what we need: `best` dies when a completing fill removes the order.
        const OrderId makerId = best->id();
        const Price executionPrice = best->price();  // the resting order's price
        const Quantity quantity =
            std::min(incoming.remainingQuantity(), best->remainingQuantity());

        auto trade = Trade::create(allocateTradeId(), makerId, id, side, executionPrice,
                                   quantity, incoming.sequence());
        if (!trade) throw std::logic_error("matching produced an invalid trade");

        // Record before mutating: a failed allocation here leaves book and incoming
        // order untouched, and the permanent history (pushed last) never contains
        // a trade that was not executed.
        report.trades.push_back(*trade);
        trades_.push_back(*trade);

        book_.fill(makerId, quantity);  // updates aggregate; removes maker + level if done
        incoming.fill(quantity);
    }

    if (incoming.remainingQuantity() > 0) {
        // Rests with its original sequence, behind everything already at its price.
        // The id was checked above and matching never adds ids, so this cannot fail.
        if (book_.add(incoming) != RejectReason::None) {
            throw std::logic_error("could not rest the unmatched remainder");
        }
    }

    report.status = incoming.status();
    report.remainingQuantity = incoming.remainingQuantity();
    report.executedQuantity = incoming.executedQuantity();
    return report;
}

}  // namespace lob
