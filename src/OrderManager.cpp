#include "OrderManager.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace lob {

namespace {

bool validSide(Side side) noexcept { return side == Side::Buy || side == Side::Sell; }

ExecutionReport reportFor(Operation operation, const OrderRecord& r, bool accepted,
                          RejectReason reason, std::vector<Trade> trades = {}) {
    ExecutionReport report;
    report.operation = operation;
    report.orderId = r.id;
    report.accepted = accepted;
    report.rejectReason = reason;
    report.cancelReason =
        (operation == Operation::Cancel && accepted) ? r.cancelReason : CancelReason::None;
    report.status = r.status;
    report.originalQuantity = r.effectiveQuantity;
    report.executedQuantity = r.executedQuantity;
    report.remainingQuantity = r.remainingQuantity;
    report.trades = std::move(trades);
    return report;
}

// Report for a request that names no known order (nothing is recorded).
ExecutionReport unrecorded(Operation operation, OrderId id, Quantity quantity,
                           RejectReason reason) {
    ExecutionReport report;
    report.operation = operation;
    report.orderId = id;
    report.accepted = false;
    report.rejectReason = reason;
    report.status = OrderStatus::Rejected;  // placeholder: there is no such order
    report.originalQuantity = quantity;
    return report;
}

}  // namespace

OrderManager::OrderManager(OrderId firstGeneratedId, TradeId firstTradeId)
    : engine_(firstTradeId), records_(), nextGeneratedId_(firstGeneratedId) {
    if (firstGeneratedId == 0) {
        throw std::invalid_argument("first generated order id must be > 0");
    }
}

// ---- Id and sequence allocation ------------------------------------------

std::optional<OrderId> OrderManager::allocateId() {
    while (!generatedIdsExhausted_) {
        const OrderId id = nextGeneratedId_;
        if (id == std::numeric_limits<OrderId>::max()) {
            generatedIdsExhausted_ = true;  // never wrap around
        } else {
            ++nextGeneratedId_;
        }
        if (records_.count(id) == 0) return id;  // skip ids a client already used
    }
    return std::nullopt;
}

bool OrderManager::allocateSequence(Sequence& out) {
    if (sequencesExhausted_) return false;
    out = nextSequence_;
    if (out == std::numeric_limits<Sequence>::max()) {
        sequencesExhausted_ = true;
    } else {
        ++nextSequence_;
    }
    return true;
}

// ---- Trade history ---------------------------------------------------------

void OrderManager::syncTrades() {
    const std::vector<Trade>& all = engine_.trades();
    while (processedTrades_ < all.size()) {
        const Trade& t = all[processedTrades_];
        const auto buyer = records_.find(t.buyOrderId());
        const auto seller = records_.find(t.sellOrderId());
        if (buyer == records_.end() || seller == records_.end()) {
            throw std::logic_error("trade refers to an order the manager does not know");
        }
        OrderRecord& b = buyer->second;
        OrderRecord& s = seller->second;
        if (b.remainingQuantity < t.quantity() || s.remainingQuantity < t.quantity() ||
            !b.isActive() || !s.isActive()) {
            throw std::logic_error("trade does not fit the lifecycle records");
        }
        // Everything that can throw (allocation) happens before anything changes,
        // so each trade is applied completely or not at all.
        for (OrderRecord* rec : {&b, &s}) {
            if (rec->tradeIndices.size() == rec->tradeIndices.capacity()) {
                rec->tradeIndices.reserve(std::max<std::size_t>(4, rec->tradeIndices.capacity() * 2));
            }
        }
        for (OrderRecord* rec : {&b, &s}) {
            rec->executedQuantity += t.quantity();
            rec->remainingQuantity -= t.quantity();
            rec->status = rec->remainingQuantity == 0 ? OrderStatus::Filled
                                                      : OrderStatus::PartiallyFilled;
            rec->tradeIndices.push_back(processedTrades_);
        }
        ++processedTrades_;
    }
}

void OrderManager::reconcileLostOrder(OrderId id) noexcept {
    try {
        syncTrades();
    } catch (...) {
        return;  // records are incomplete; do not guess
    }
    const auto it = records_.find(id);
    if (it == records_.end()) return;
    OrderRecord& r = it->second;
    if (r.isActive() && !engine_.book().contains(id)) {
        r.cancelledQuantity = r.remainingQuantity;
        r.remainingQuantity = 0;
        r.status = OrderStatus::Cancelled;
        r.cancelReason = CancelReason::SystemFailure;
    }
}

// ---- Submission --------------------------------------------------------------

ExecutionReport OrderManager::submit(Side side, Price price, Quantity quantity) {
    syncTrades();
    if (!validSide(side)) return unrecorded(Operation::Add, 0, quantity, RejectReason::InvalidSide);
    const std::optional<OrderId> id = allocateId();
    if (!id) return unrecorded(Operation::Add, 0, quantity, RejectReason::IdSpaceExhausted);
    return process(*id, side, price, quantity);
}

ExecutionReport OrderManager::submit(OrderId id, Side side, Price price, Quantity quantity) {
    syncTrades();
    if (!validSide(side)) return unrecorded(Operation::Add, id, quantity, RejectReason::InvalidSide);
    if (id == 0) return unrecorded(Operation::Add, id, quantity, RejectReason::InvalidOrderId);
    if (knows(id)) return unrecorded(Operation::Add, id, quantity, RejectReason::DuplicateOrderId);
    return process(id, side, price, quantity);
}

// Precondition: `id` is non-zero and has never been recorded.
ExecutionReport OrderManager::process(OrderId id, Side side, Price price, Quantity quantity) {
    OrderRecord rec;
    rec.id = id;
    rec.side = side;
    rec.originalPrice = price;
    rec.price = price;
    rec.submittedQuantity = quantity;
    rec.effectiveQuantity = quantity;

    RejectReason reason = Order::validate(id, price, quantity);
    Sequence sequence = 0;
    if (reason == RejectReason::None && !allocateSequence(sequence)) {
        reason = RejectReason::IdSpaceExhausted;
    }
    if (reason != RejectReason::None) {
        // The id is usable, so the rejection is part of history and the id retired.
        rec.status = OrderStatus::Rejected;
        rec.rejectReason = reason;
        const OrderRecord& stored = records_.emplace(id, std::move(rec)).first->second;
        return reportFor(Operation::Add, stored, false, reason);
    }

    rec.submissionSequence = sequence;
    rec.sequence = sequence;
    rec.status = OrderStatus::New;
    rec.remainingQuantity = quantity;
    // The record exists before the order can reach the book, so a resting order
    // never lacks one. If emplace throws, nothing has happened yet.
    OrderRecord& r = records_.emplace(id, std::move(rec)).first->second;

    ExecutionReport engineReport;
    try {
        engineReport = engine_.submit(*Order::create(id, side, OrderType::Limit, price,
                                                      quantity, sequence));
    } catch (const std::overflow_error&) {
        // Raised before the engine changed anything: the order never existed.
        r.status = OrderStatus::Rejected;
        r.rejectReason = RejectReason::TradeIdsExhausted;
        r.remainingQuantity = 0;
        return reportFor(Operation::Add, r, false, RejectReason::TradeIdsExhausted);
    } catch (...) {
        reconcileLostOrder(id);
        throw;
    }
    if (!engineReport.accepted) {  // not reachable with the checks above; kept for safety
        r.status = OrderStatus::Rejected;
        r.rejectReason = engineReport.rejectReason;
        r.remainingQuantity = 0;
        return reportFor(Operation::Add, r, false, engineReport.rejectReason);
    }
    syncTrades();
    return reportFor(Operation::Add, r, true, RejectReason::None, std::move(engineReport.trades));
}

// ---- Cancellation ---------------------------------------------------------------

ExecutionReport OrderManager::cancel(OrderId id) {
    syncTrades();
    const auto it = records_.find(id);
    if (it == records_.end()) return unrecorded(Operation::Cancel, id, 0, RejectReason::OrderNotFound);
    OrderRecord& r = it->second;
    if (!r.isActive()) return reportFor(Operation::Cancel, r, false, RejectReason::OrderNotActive);

    std::optional<Order> removed = engine_.cancel(id);
    if (!removed) throw std::logic_error("active record has no order in the book");
    // From here on nothing can throw: the record update is plain assignment.
    r.cancelledQuantity = removed->remainingQuantity();
    r.remainingQuantity = 0;
    r.status = OrderStatus::Cancelled;
    r.cancelReason = CancelReason::UserRequested;
    return reportFor(Operation::Cancel, r, true, RejectReason::None);
}

// ---- Modification -----------------------------------------------------------------

ExecutionReport OrderManager::modify(OrderId id, std::optional<Price> newPrice,
                                     std::optional<Quantity> newQuantity) {
    syncTrades();
    const auto it = records_.find(id);
    if (it == records_.end()) return unrecorded(Operation::Modify, id, 0, RejectReason::OrderNotFound);
    OrderRecord& r = it->second;
    const auto fail = [&r](RejectReason why) { return reportFor(Operation::Modify, r, false, why); };

    if (!r.isActive()) return fail(RejectReason::OrderNotActive);
    if (newPrice && !isValidPrice(*newPrice)) return fail(RejectReason::InvalidPrice);
    if (newQuantity && !isValidQuantity(*newQuantity)) return fail(RejectReason::InvalidQuantity);

    const Order* current = engine_.book().find(id);
    if (current == nullptr) throw std::logic_error("active record has no order in the book");
    const Price currentPrice = current->price();
    const Quantity currentRemaining = current->remainingQuantity();
    const Price price = newPrice.value_or(currentPrice);
    const Quantity quantity = newQuantity.value_or(currentRemaining);

    if (price == currentPrice && quantity == currentRemaining) return fail(RejectReason::NoChange);

    if (price == currentPrice && quantity < currentRemaining) {
        // Pure reduction: stays where it is in the queue, keeps time priority.
        engine_.reduce(id, quantity);
        r.remainingQuantity = quantity;
        r.effectiveQuantity = r.executedQuantity + quantity;
        return reportFor(Operation::Modify, r, true, RejectReason::None);
    }

    // Price change or quantity increase: priority is lost (cancel/replace).
    if (quantity > std::numeric_limits<Quantity>::max() - r.executedQuantity) {
        return fail(RejectReason::InvalidQuantity);
    }
    Sequence sequence = 0;
    if (!allocateSequence(sequence)) return fail(RejectReason::IdSpaceExhausted);

    const auto applyTerms = [&] {
        r.price = price;
        r.sequence = sequence;
        r.remainingQuantity = quantity;
        r.effectiveQuantity = r.executedQuantity + quantity;
    };

    const std::size_t tradesBefore = engine_.trades().size();
    ExecutionReport engineReport;
    try {
        engineReport = engine_.replace(id, price, quantity, sequence);
    } catch (const std::overflow_error&) {
        // Thrown before the engine touched the book: the order is exactly as it was.
        return fail(RejectReason::TradeIdsExhausted);
    } catch (...) {
        // Allocation failure part-way through. If the replacement already traded, its
        // new terms apply to those trades; if it never got that far the old terms stand.
        if (engine_.trades().size() > tradesBefore) applyTerms();
        reconcileLostOrder(id);
        throw;
    }
    if (!engineReport.accepted) return fail(engineReport.rejectReason);  // defensive
    applyTerms();
    syncTrades();  // subtracts any executions from the new remaining quantity
    return reportFor(Operation::Modify, r, true, RejectReason::None, std::move(engineReport.trades));
}

// ---- Queries ----------------------------------------------------------------------

std::optional<OrderRecord> OrderManager::order(OrderId id) const {
    const auto it = records_.find(id);
    if (it == records_.end()) return std::nullopt;
    return it->second;
}

std::vector<Trade> OrderManager::tradesFor(OrderId id) const {
    std::vector<Trade> out;
    const auto it = records_.find(id);
    if (it == records_.end()) return out;
    const std::vector<Trade>& all = engine_.trades();
    out.reserve(it->second.tradeIndices.size());
    for (const std::size_t index : it->second.tradeIndices) out.push_back(all.at(index));
    return out;
}

// ---- Consistency audit ----------------------------------------------------------------

InvariantReport OrderManager::checkConsistency() const {
    InvariantReport report = engine_.book().checkInvariants();
    std::vector<std::string>& v = report.violations;
    const OrderBook& book = engine_.book();
    const std::vector<Trade>& trades = engine_.trades();

    if (processedTrades_ != trades.size()) {
        v.push_back("trade cursor is behind the engine's trade list");
    }

    std::vector<int> timesRecorded(trades.size(), 0);
    std::size_t activeRecords = 0;

    for (const auto& entry : records_) {
        const OrderRecord& r = entry.second;
        const std::string who = "order " + std::to_string(entry.first) + ": ";
        if (r.id != entry.first) v.push_back(who + "record id differs from its key");

        if (r.executedQuantity < 0 || r.remainingQuantity < 0 || r.cancelledQuantity < 0) {
            v.push_back(who + "negative quantity");
        }
        if (r.status != OrderStatus::Rejected &&
            r.executedQuantity + r.remainingQuantity + r.cancelledQuantity !=
                r.effectiveQuantity) {
            v.push_back(who + "executed + remaining + cancelled != effective quantity");
        }

        switch (r.status) {
            case OrderStatus::New:
                if (r.executedQuantity != 0 || r.remainingQuantity <= 0) {
                    v.push_back(who + "NEW but executed != 0 or nothing remaining");
                }
                break;
            case OrderStatus::PartiallyFilled:
                if (r.executedQuantity <= 0 || r.remainingQuantity <= 0) {
                    v.push_back(who + "PARTIALLY_FILLED without both executed and remaining");
                }
                break;
            case OrderStatus::Filled:
                if (r.remainingQuantity != 0 || r.executedQuantity <= 0 ||
                    r.executedQuantity != r.effectiveQuantity) {
                    v.push_back(who + "FILLED but not fully executed");
                }
                break;
            case OrderStatus::Cancelled:
                if (r.remainingQuantity != 0 || r.cancelledQuantity <= 0 ||
                    r.cancelReason == CancelReason::None) {
                    v.push_back(who + "CANCELLED without a cancelled quantity and reason");
                }
                break;
            case OrderStatus::Rejected:
                if (r.executedQuantity != 0 || r.remainingQuantity != 0 ||
                    !r.tradeIndices.empty() || r.rejectReason == RejectReason::None) {
                    v.push_back(who + "REJECTED but has executions, quantity or no reason");
                }
                break;
        }

        // Record vs. book: active <=> resting, with identical terms.
        const Order* resting = book.find(entry.first);
        if (r.isActive()) {
            ++activeRecords;
            if (resting == nullptr) {
                v.push_back(who + "active record but the order is not in the book");
            } else if (resting->side() != r.side || resting->price() != r.price ||
                       resting->remainingQuantity() != r.remainingQuantity ||
                       resting->executedQuantity() != r.executedQuantity ||
                       resting->originalQuantity() != r.effectiveQuantity ||
                       resting->sequence() != r.sequence || resting->status() != r.status) {
                v.push_back(who + "record differs from the resting order");
            }
        } else if (resting != nullptr) {
            v.push_back(who + "terminal record but the order is still in the book");
        }

        // Trade associations: valid, involve this order, increasing, sum to executed.
        Quantity traded = 0;
        bool havePrev = false;
        std::size_t prev = 0;
        for (const std::size_t index : r.tradeIndices) {
            if (index >= trades.size()) {
                v.push_back(who + "trade index out of range");
                continue;
            }
            const Trade& t = trades[index];
            if (t.buyOrderId() != entry.first && t.sellOrderId() != entry.first) {
                v.push_back(who + "lists a trade it did not take part in");
            }
            if (havePrev && index <= prev) v.push_back(who + "trade indices not strictly increasing");
            havePrev = true;
            prev = index;
            ++timesRecorded[index];
            traded += t.quantity();
        }
        if (traded != r.executedQuantity) {
            v.push_back(who + "executed quantity differs from the sum of its trades");
        }
    }

    for (std::size_t i = 0; i < timesRecorded.size(); ++i) {
        if (timesRecorded[i] != 2) {
            v.push_back("trade " + std::to_string(i) + " recorded " +
                        std::to_string(timesRecorded[i]) +
                        " times (expected exactly once for buyer and once for seller)");
        }
    }
    if (activeRecords != book.orderCount()) {
        v.push_back("active records (" + std::to_string(activeRecords) + ") != orders in book (" +
                    std::to_string(book.orderCount()) + ")");
    }
    return report;
}

}  // namespace lob
