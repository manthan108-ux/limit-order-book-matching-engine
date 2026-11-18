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

ExecutionReport rejection(Operation operation, OrderId id, Quantity originalQuantity,
                          RejectReason reason) {
    ExecutionReport r;
    r.operation = operation;
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
    if (invalid != RejectReason::None) {
        return rejection(Operation::Add, id, quantity, invalid);
    }
    return submit(*Order::create(id, side, OrderType::Limit, price, quantity, sequence));
}

bool MatchingEngine::restingWouldOverflow(Side side, Price price, Quantity quantity,
                                          Quantity alreadyCounted) const {
    Quantity levelTotal = 0;
    if (side == Side::Buy) {
        const auto it = book_.bids().find(price);
        if (it != book_.bids().end()) levelTotal = it->second.totalQuantity();
    } else {
        const auto it = book_.asks().find(price);
        if (it != book_.asks().end()) levelTotal = it->second.totalQuantity();
    }
    levelTotal -= alreadyCounted;  // never negative: it is part of that total
    return levelTotal > std::numeric_limits<Quantity>::max() - quantity;
}

void MatchingEngine::ensureTradeIds(const Order& incoming) const {
    // Fast path: enough ids for even one trade per resting order. Otherwise
    // count exactly, since ids this scarce are an extreme corner case.
    if (tradeIdsRemaining() < book_.orderCount()) {
        const std::uint64_t needed = incoming.side() == Side::Buy
                                         ? countExecutions(book_.asks(), incoming)
                                         : countExecutions(book_.bids(), incoming);
        if (needed > tradeIdsRemaining()) {
            throw std::overflow_error("trade id space cannot cover this order's executions");
        }
    }
}

ExecutionReport MatchingEngine::submit(Order incoming) {
    const OrderId id = incoming.id();
    const Quantity original = incoming.originalQuantity();

    if (book_.contains(id)) {
        return rejection(Operation::Add, id, original, RejectReason::DuplicateOrderId);
    }
    if (!incoming.isActive()) {
        return rejection(Operation::Add, id, original, RejectReason::OrderNotActive);
    }
    if (incoming.remainingQuantity() <= 0) {
        return rejection(Operation::Add, id, original, RejectReason::InvalidQuantity);
    }
    if (restingWouldOverflow(incoming.side(), incoming.price(), incoming.remainingQuantity(), 0)) {
        return rejection(Operation::Add, id, original, RejectReason::QuantityOverflow);
    }
    ensureTradeIds(incoming);  // fail before any mutation
    return execute(std::move(incoming), Operation::Add);
}

ExecutionReport MatchingEngine::replace(OrderId id, Price newPrice, Quantity newRemaining,
                                        Sequence newSequence) {
    const Order* current = book_.find(id);
    if (current == nullptr) {
        return rejection(Operation::Modify, id, 0, RejectReason::OrderNotFound);
    }
    const RejectReason invalid = Order::validate(id, newPrice, newRemaining);
    if (invalid != RejectReason::None) {
        return rejection(Operation::Modify, id, newRemaining, invalid);
    }
    if (newRemaining > std::numeric_limits<Quantity>::max() - current->executedQuantity()) {
        return rejection(Operation::Modify, id, newRemaining, RejectReason::InvalidQuantity);
    }
    // Its own quantity leaves the old level when it is replaced, so it only counts against
    // the new level if that is the same level.
    if (restingWouldOverflow(current->side(), newPrice, newRemaining,
                             current->price() == newPrice ? current->remainingQuantity() : 0)) {
        return rejection(Operation::Modify, id, newRemaining, RejectReason::QuantityOverflow);
    }
    // Everything that can fail logically is checked while the order is still
    // safely resting; only then is it taken out.
    Order replacement = current->replaced(newPrice, newRemaining, newSequence);
    ensureTradeIds(replacement);  // reads the opposite side only, unaffected by the removal
    book_.extract(id);
    return execute(std::move(replacement), Operation::Modify);
}

ExecutionReport MatchingEngine::execute(Order incoming, Operation operation) {
    const OrderId id = incoming.id();
    const Quantity original = incoming.originalQuantity();
    const Side side = incoming.side();
    const Side opposite = lob::opposite(side);

    ExecutionReport report;
    report.operation = operation;
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
