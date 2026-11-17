#pragma once

#include <optional>

#include "OrderTypes.h"

namespace lob {

// A limit order. Instances can only be obtained through Order::create, so an
// Order that exists is always valid:
//   id > 0, price > 0, originalQuantity > 0,
//   0 <= remainingQuantity <= originalQuantity,
//   executedQuantity == originalQuantity - remainingQuantity,
//   status is consistent with the quantities (see below).
//
// Status/quantity consistency:
//   New              executed == 0
//   PartiallyFilled  0 < executed < original
//   Filled           remaining == 0
//   Cancelled        remaining unchanged at the time of cancellation
//
// Mutators are the only way to change state, and every state change goes
// through canTransition(). Mutators throw on misuse (a programming error in
// the caller) rather than silently corrupting the order; callers are expected
// to check isActive()/remainingQuantity() first.
class Order {
public:
    // Returns RejectReason::None if the parameters describe a valid order,
    // otherwise the first failing check (id, then price, then quantity).
    static RejectReason validate(OrderId id, Price price, Quantity quantity) noexcept;

    // Builds an order in status NEW, or std::nullopt if validate() fails.
    // `sequence` is the arrival counter assigned by the engine and defines
    // time priority; the domain model does not read a clock itself so tests
    // and replays are deterministic.
    static std::optional<Order> create(OrderId id, Side side, OrderType type,
                                       Price price, Quantity quantity,
                                       Sequence sequence);

    OrderId id() const noexcept { return id_; }
    Side side() const noexcept { return side_; }
    OrderType type() const noexcept { return type_; }
    Price price() const noexcept { return price_; }
    Quantity originalQuantity() const noexcept { return originalQuantity_; }
    Quantity remainingQuantity() const noexcept { return remainingQuantity_; }
    Quantity executedQuantity() const noexcept {
        return originalQuantity_ - remainingQuantity_;
    }
    Sequence sequence() const noexcept { return sequence_; }
    OrderStatus status() const noexcept { return status_; }

    bool isActive() const noexcept { return lob::isActive(status_); }

    // Executes `quantity` against this order.
    // Throws std::logic_error if the order is not active,
    // std::invalid_argument if quantity <= 0 or quantity > remainingQuantity.
    // Status becomes PARTIALLY_FILLED, or FILLED when nothing remains.
    void fill(Quantity quantity);

    // ---- Amendments (used by order modification) --------------------------
    // Amending never touches the cumulative executed quantity. The "original"
    // quantity of an amended order is therefore its effective total:
    //   originalQuantity == executedQuantity + remainingQuantity   (always)

    // Lowers the remaining quantity in place (the order keeps its identity,
    // price and sequence). Requires 0 < newRemaining < remainingQuantity;
    // throws std::logic_error if inactive, std::invalid_argument otherwise.
    // Nothing changes when it throws.
    void reduceTo(Quantity newRemaining);

    // A copy of this order with a new price, remaining quantity and sequence
    // (time priority); id, side, type, status and executed quantity carry over.
    // Used for changes that lose priority. This order is not modified.
    // Throws std::logic_error if inactive; std::invalid_argument if the price or
    // quantity is invalid or executed + newRemaining would overflow.
    Order replaced(Price newPrice, Quantity newRemaining, Sequence newSequence) const;

    // NEW / PARTIALLY_FILLED -> CANCELLED. Throws std::logic_error otherwise.
    void cancel();

    // NEW -> REJECTED. Throws std::logic_error otherwise.
    void reject();

private:
    Order(OrderId id, Side side, OrderType type, Price price, Quantity quantity,
          Sequence sequence) noexcept;

    void transitionTo(OrderStatus next);

    OrderId id_;
    Side side_;
    OrderType type_;
    Price price_;
    Quantity originalQuantity_;
    Quantity remainingQuantity_;
    Sequence sequence_;
    OrderStatus status_;
};

}  // namespace lob
