#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Order.h"
#include "OrderTypes.h"
#include "PriceLevel.h"

namespace lob {

// Read-only snapshot of one price level.
struct LevelSummary {
    Price price;
    Quantity totalQuantity;
    std::size_t orderCount;
};

struct MarketDepth {
    std::vector<LevelSummary> bids;  // best (highest) first
    std::vector<LevelSummary> asks;  // best (lowest) first
};

// Exact midpoint of two integer tick prices without floating point:
//   mid = ticks + (halfTick ? 0.5 : 0), where ticks is floor((bid + ask) / 2).
struct Midpoint {
    Price ticks;
    bool halfTick;
};

// Result of OrderBook::checkInvariants(). Every violated rule is described, so
// a failing test or debug session says *what* broke, not just that something did.
struct InvariantReport {
    std::vector<std::string> violations;
    bool ok() const noexcept { return violations.empty(); }
    explicit operator bool() const noexcept { return ok(); }
};

// The resting state of the market: bids and asks as ordered maps of FIFO
// PriceLevels, plus a global id -> location index.
//
// Responsibilities (Phase 3): store, index, cancel and query resting orders.
// It does NOT match. Crossing orders may rest side by side until the matching
// engine (Phase 4) is layered on top.
//
// Ownership: each PriceLevel owns its orders; the book owns the levels; the
// lookup stores non-owning list iterators ("handles") into those levels.
//
// Handle safety: a handle is invalidated only when its order leaves its level.
// Every path that removes an order (cancel, and later fills) erases the lookup
// entry in the same operation, and a level is erased only when it is empty,
// i.e. when no handle can refer into it. std::map nodes are never relocated,
// so inserting/erasing *other* levels cannot affect a stored handle. The book
// therefore never holds a stale handle and never dereferences an unchecked one.
//
// The class is move-only: std::list/std::map moves keep iterators valid, but a
// copy would leave the lookup pointing into the *source* book's lists.
//
// Id policy: ids are unique among orders currently resting. After an order is
// cancelled its id may be added again. Remembering retired ids is the order
// manager's concern (Phase 5), not the book's.
//
// Sequence policy: callers must submit orders with non-decreasing `sequence`
// values in arrival order; FIFO position within a level is the insertion
// order and checkInvariants() verifies it agrees with `sequence`.
//
// Single-threaded.
class OrderBook {
public:
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    OrderBook() = default;
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    // ---- Mutations -------------------------------------------------------

    // Rests `order` at the back of its price level (creating the level if
    // needed). Returns RejectReason::None on success. The book is unchanged
    // on rejection:
    //   DuplicateOrderId  an order with this id is already resting (either side)
    //   OrderNotActive    the order is filled / cancelled / rejected
    //   InvalidQuantity   the order has no remaining quantity
    // Strong exception guarantee (if allocation throws, nothing was added).
    RejectReason add(Order order);

    // Removes the resting order `id`, marks it CANCELLED and returns it.
    // Returns std::nullopt for an unknown id (including one already cancelled).
    std::optional<Order> cancel(OrderId id);

    // ---- Order queries ---------------------------------------------------

    bool contains(OrderId id) const noexcept { return lookup_.count(id) != 0; }

    // Non-owning, read-only view of a resting order, or nullptr if the id is
    // not resting. The pointer is valid until that order leaves the book.
    const Order* find(OrderId id) const;

    std::size_t orderCount() const noexcept { return lookup_.size(); }
    std::size_t bidLevelCount() const noexcept { return bids_.size(); }
    std::size_t askLevelCount() const noexcept { return asks_.size(); }
    bool empty() const noexcept { return lookup_.empty(); }

    // ---- Market data -----------------------------------------------------

    std::optional<LevelSummary> bestBid() const;
    std::optional<LevelSummary> bestAsk() const;

    // bestAsk - bestBid. Zero or negative only while the book is crossed
    // (possible until Phase 4 matching exists). nullopt if either side is empty.
    // Cannot overflow: both prices are positive.
    std::optional<Price> spread() const;

    // Exact midpoint, see Midpoint. Computed without forming bid + ask, so it
    // cannot overflow even for prices near the Price maximum.
    std::optional<Midpoint> midPrice() const;

    // Top `levels` price levels per side. Fewer are returned if fewer exist.
    MarketDepth depth(std::size_t levels) const;

    // Read-only access to the sides (for book display and tests). Prices are
    // ordered best-first; each level exposes its orders in FIFO order.
    const BidMap& bids() const noexcept { return bids_; }
    const AskMap& asks() const noexcept { return asks_; }

    // ---- Validation ------------------------------------------------------

    // Full O(n) consistency audit, see docs/ARCHITECTURE.md for the rule list.
    InvariantReport checkInvariants() const;

private:
    struct OrderLocation {
        Side side;
        Price price;
        PriceLevel::OrderHandle handle;
    };
    using Lookup = std::unordered_map<OrderId, OrderLocation>;

    template <typename Map>
    RejectReason addTo(Map& side, Order order);

    template <typename Map>
    Order removeFrom(Map& side, Lookup::iterator entry);

    template <typename Map>
    void checkSide(const Map& side, Side which, std::vector<std::string>& out,
                   std::size_t& ordersSeen) const;

    BidMap bids_;
    AskMap asks_;
    Lookup lookup_;
};

}  // namespace lob
