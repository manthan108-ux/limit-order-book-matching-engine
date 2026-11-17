#include "PriceLevel.h"

#include <iterator>
#include <stdexcept>
#include <utility>

namespace lob {

PriceLevel::PriceLevel(Price price) : price_(price), totalQuantity_(0), orders_() {
    if (!isValidPrice(price)) {
        throw std::invalid_argument("price level requires a positive price");
    }
}

const Order& PriceLevel::front() const {
    if (orders_.empty()) throw std::logic_error("front() on empty price level");
    return orders_.front();
}

PriceLevel::OrderHandle PriceLevel::add(Order order) {
    if (order.price() != price_) {
        throw std::invalid_argument("order price does not match price level");
    }
    if (!order.isActive() || order.remainingQuantity() <= 0) {
        throw std::invalid_argument("only active orders with remaining quantity can rest");
    }
    const Quantity remaining = order.remainingQuantity();
    orders_.push_back(std::move(order));  // strong guarantee: throws before any change
    totalQuantity_ += remaining;
    return std::prev(orders_.cend());
}

Order PriceLevel::remove(OrderHandle handle) {
    if (handle == orders_.cend()) {
        throw std::out_of_range("remove() with end() handle");
    }
    Queue::iterator it = orders_.erase(handle, handle);  // const -> mutable, no-op erase
    Order removed = std::move(*it);
    totalQuantity_ -= removed.remainingQuantity();
    orders_.erase(it);
    return removed;
}

Order PriceLevel::popFront() {
    if (orders_.empty()) throw std::logic_error("popFront() on empty price level");
    return remove(orders_.cbegin());
}

PriceLevel::FillResult PriceLevel::fill(OrderHandle handle, Quantity quantity) {
    if (handle == orders_.cend()) {
        throw std::out_of_range("fill() with end() handle");
    }
    // erase(h, h) erases nothing but converts a const_iterator to an iterator,
    // which is the standard way to get mutable access from a const handle.
    Queue::iterator it = orders_.erase(handle, handle);
    it->fill(quantity);  // throws without modifying the order on bad input
    totalQuantity_ -= quantity;
    if (it->remainingQuantity() == 0) {
        orders_.erase(it);
        return FillResult::Filled;
    }
    return FillResult::PartiallyFilled;
}

bool PriceLevel::checkInvariants() const noexcept {
    Quantity sum = 0;
    for (const Order& o : orders_) {
        if (o.price() != price_ || !o.isActive() || o.remainingQuantity() <= 0) {
            return false;
        }
        sum += o.remainingQuantity();
    }
    return sum == totalQuantity_ && totalQuantity_ >= 0;
}

}  // namespace lob
