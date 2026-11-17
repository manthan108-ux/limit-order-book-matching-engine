#pragma once

#include <cstddef>
#include <list>

#include "Order.h"
#include "OrderTypes.h"

namespace lob {

// All resting orders at a single price, in FIFO (time-priority) order.
//
// Ownership: the level *owns* its orders by value inside a std::list. There
// are no raw pointers and no shared ownership.
//
// Iterator strategy: std::list never invalidates iterators/references to the
// other elements when one is erased or when elements are appended. add()
// returns an OrderHandle (a list iterator) that the OrderBook will store in
// its id -> location map; remove(handle) then erases from the middle of the
// queue in O(1). A handle is invalidated only by removal of the order it
// refers to (including via popFront() or a completing fill()).
//
// Aggregate-quantity integrity: orders are exposed read-only (front(),
// begin()/end()). The only way to change an order's remaining quantity while
// it rests here is fill(), which updates the aggregate in the same step. The
// aggregate therefore cannot drift from the sum of remaining quantities.
//
// Error handling: violating a documented precondition throws (std::logic_error
// or std::invalid_argument); it never leaves the level in a changed state.
//
// Not checked (cost / layering): that a handle belongs to *this* level, and
// that order ids are unique. The OrderBook owns those guarantees.
class PriceLevel {
public:
    using Queue = std::list<Order>;
    using OrderHandle = Queue::const_iterator;

    enum class FillResult { PartiallyFilled, Filled };

    // Throws std::invalid_argument if price is not a valid price.
    explicit PriceLevel(Price price);

    Price price() const noexcept { return price_; }
    bool empty() const noexcept { return orders_.empty(); }
    std::size_t orderCount() const noexcept { return orders_.size(); }
    // Sum of remainingQuantity of all orders in the level.
    Quantity totalQuantity() const noexcept { return totalQuantity_; }

    // Oldest order. Throws std::logic_error if the level is empty.
    const Order& front() const;

    // Read-only iteration in FIFO order (for book display and invariant checks).
    Queue::const_iterator begin() const noexcept { return orders_.begin(); }
    Queue::const_iterator end() const noexcept { return orders_.end(); }

    // Appends at the back (lowest time priority) and returns a stable handle.
    // Throws std::invalid_argument if the order's price differs from the
    // level's, or the order is not active / has no remaining quantity.
    OrderHandle add(Order order);

    // Removes and returns the order referred to by `handle`, from anywhere in
    // the queue. Throws std::out_of_range if handle == end(). The returned
    // order is unchanged (status is not modified); the caller decides whether
    // it was cancelled, moved, etc.
    Order remove(OrderHandle handle);

    // Removes and returns the oldest order. Throws std::logic_error if empty.
    Order popFront();

    // Executes `quantity` against the referenced resting order, updating the
    // aggregate. If this fills the order completely it is removed from the
    // level (a filled order must never stay resting) and `handle` is
    // invalidated. Throws std::out_of_range if handle == end(), or the
    // Order::fill exceptions for a bad quantity (level left unchanged).
    FillResult fill(OrderHandle handle, Quantity quantity);

    // Recomputes the aggregate from the orders and compares it to the cached
    // one; also checks every order is active, positive and at this price.
    // O(n); intended for tests and the OrderBook invariant checker.
    bool checkInvariants() const noexcept;

private:
    Price price_;
    Quantity totalQuantity_;
    Queue orders_;
};

}  // namespace lob
