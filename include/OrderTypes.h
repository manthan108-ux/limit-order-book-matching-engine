#pragma once

#include <cstdint>
#include <string>

namespace lob {

// ---- Fundamental scalar types -------------------------------------------
//
// Prices are integer ticks (e.g. 1 tick = $0.01), never floating point, so
// comparisons and arithmetic are exact. Price and Quantity are deliberately
// *signed*: untrusted input such as "-5" must be representable so that
// validation can reject it explicitly rather than silently wrapping around.
using OrderId  = std::uint64_t;
using TradeId  = std::uint64_t;
using Price    = std::int64_t;   // ticks
using Quantity = std::int64_t;   // units
using Sequence = std::uint64_t;  // monotonically increasing event counter

// ---- Enumerations ---------------------------------------------------------

enum class Side : std::uint8_t { Buy, Sell };

// Only LIMIT is required now; the enum leaves room for MARKET / IOC / FOK.
enum class OrderType : std::uint8_t { Limit };

enum class OrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected
};

enum class Operation : std::uint8_t { Add, Cancel, Modify };

enum class RejectReason : std::uint8_t {
    None,
    DuplicateOrderId,
    InvalidPrice,
    InvalidQuantity,
    OrderNotFound,
    OrderNotActive,   // already filled / cancelled / rejected
    NoChange          // modify request changes nothing
};

// ---- Helpers --------------------------------------------------------------

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Filled, Cancelled and Rejected orders can never change state again and must
// never be present in the book.
constexpr bool isTerminal(OrderStatus s) noexcept {
    return s == OrderStatus::Filled || s == OrderStatus::Cancelled ||
           s == OrderStatus::Rejected;
}

constexpr bool isActive(OrderStatus s) noexcept { return !isTerminal(s); }

// Legal order lifecycle transitions:
//   New             -> PartiallyFilled | Filled | Cancelled | Rejected
//   PartiallyFilled -> PartiallyFilled | Filled | Cancelled
//   Filled / Cancelled / Rejected -> (terminal, nothing)
constexpr bool canTransition(OrderStatus from, OrderStatus to) noexcept {
    switch (from) {
        case OrderStatus::New:
            return to == OrderStatus::PartiallyFilled || to == OrderStatus::Filled ||
                   to == OrderStatus::Cancelled || to == OrderStatus::Rejected;
        case OrderStatus::PartiallyFilled:
            return to == OrderStatus::PartiallyFilled || to == OrderStatus::Filled ||
                   to == OrderStatus::Cancelled;
        default:
            return false;
    }
}

// A limit order must have a strictly positive tick price and quantity.
constexpr bool isValidPrice(Price p) noexcept { return p > 0; }
constexpr bool isValidQuantity(Quantity q) noexcept { return q > 0; }

inline std::string toString(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }

inline std::string toString(OrderType) { return "LIMIT"; }

inline std::string toString(OrderStatus s) {
    switch (s) {
        case OrderStatus::New:             return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderStatus::Filled:          return "FILLED";
        case OrderStatus::Cancelled:       return "CANCELLED";
        case OrderStatus::Rejected:        return "REJECTED";
    }
    return "UNKNOWN";
}

inline std::string toString(Operation o) {
    switch (o) {
        case Operation::Add:    return "ADD";
        case Operation::Cancel: return "CANCEL";
        case Operation::Modify: return "MODIFY";
    }
    return "UNKNOWN";
}

inline std::string toString(RejectReason r) {
    switch (r) {
        case RejectReason::None:             return "none";
        case RejectReason::DuplicateOrderId: return "duplicate order id";
        case RejectReason::InvalidPrice:     return "invalid price (must be > 0)";
        case RejectReason::InvalidQuantity:  return "invalid quantity (must be > 0)";
        case RejectReason::OrderNotFound:    return "order not found";
        case RejectReason::OrderNotActive:   return "order is not active";
        case RejectReason::NoChange:         return "modification changes nothing";
    }
    return "unknown";
}

}  // namespace lob
