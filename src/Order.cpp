#include "Order.h"

#include <limits>
#include <stdexcept>

namespace lob {

RejectReason Order::validate(OrderId id, Price price, Quantity quantity) noexcept {
    if (id == 0) return RejectReason::InvalidOrderId;
    if (!isValidPrice(price)) return RejectReason::InvalidPrice;
    if (!isValidQuantity(quantity)) return RejectReason::InvalidQuantity;
    return RejectReason::None;
}

std::optional<Order> Order::create(OrderId id, Side side, OrderType type, Price price,
                                   Quantity quantity, Sequence sequence) {
    if (validate(id, price, quantity) != RejectReason::None) return std::nullopt;
    return Order(id, side, type, price, quantity, sequence);
}

Order::Order(OrderId id, Side side, OrderType type, Price price, Quantity quantity,
             Sequence sequence) noexcept
    : id_(id),
      side_(side),
      type_(type),
      price_(price),
      originalQuantity_(quantity),
      remainingQuantity_(quantity),
      sequence_(sequence),
      status_(OrderStatus::New) {}

void Order::transitionTo(OrderStatus next) {
    if (!canTransition(status_, next)) {
        throw std::logic_error("illegal order state transition " + toString(status_) +
                               " -> " + toString(next));
    }
    status_ = next;
}

void Order::fill(Quantity quantity) {
    if (!isActive()) {
        throw std::logic_error("cannot fill order in state " + toString(status_));
    }
    if (quantity <= 0 || quantity > remainingQuantity_) {
        throw std::invalid_argument("fill quantity must be in [1, remainingQuantity]");
    }
    // Validate the transition before touching the quantity so a failure
    // cannot leave the order half-updated.
    const bool completes = quantity == remainingQuantity_;
    transitionTo(completes ? OrderStatus::Filled : OrderStatus::PartiallyFilled);
    remainingQuantity_ -= quantity;
}

void Order::reduceTo(Quantity newRemaining) {
    if (!isActive()) {
        throw std::logic_error("cannot amend order in state " + toString(status_));
    }
    if (newRemaining <= 0 || newRemaining >= remainingQuantity_) {
        throw std::invalid_argument("reduceTo requires 0 < newRemaining < remainingQuantity");
    }
    originalQuantity_ -= remainingQuantity_ - newRemaining;  // executed stays the same
    remainingQuantity_ = newRemaining;
}

Order Order::replaced(Price newPrice, Quantity newRemaining, Sequence newSequence) const {
    if (!isActive()) {
        throw std::logic_error("cannot amend order in state " + toString(status_));
    }
    if (!isValidPrice(newPrice) || !isValidQuantity(newRemaining) ||
        newRemaining > std::numeric_limits<Quantity>::max() - executedQuantity()) {
        throw std::invalid_argument("invalid replacement price or quantity");
    }
    Order copy = *this;
    copy.price_ = newPrice;
    copy.sequence_ = newSequence;
    copy.remainingQuantity_ = newRemaining;
    copy.originalQuantity_ = executedQuantity() + newRemaining;
    return copy;
}

void Order::cancel() { transitionTo(OrderStatus::Cancelled); }

void Order::reject() { transitionTo(OrderStatus::Rejected); }

}  // namespace lob
