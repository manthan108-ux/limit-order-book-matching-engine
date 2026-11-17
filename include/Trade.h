#pragma once

#include <optional>

#include "OrderTypes.h"

namespace lob {

// An immutable record of one execution between a resting (maker) order and an
// incoming (taker) order. There are no setters; a Trade can only be built via
// Trade::create, so every Trade in existence is valid:
//   tradeId > 0, makerOrderId > 0, takerOrderId > 0, maker != taker,
//   price > 0, quantity > 0.
//
// The taker side determines which of the two orders was the buyer, which is
// what lets every trade be traced back to its buy and sell order.
class Trade {
public:
    // True if the arguments describe a valid trade.
    static bool isValid(TradeId tradeId, OrderId makerOrderId, OrderId takerOrderId,
                        Price price, Quantity quantity) noexcept;

    // std::nullopt if isValid() is false. The caller (the matching engine) is
    // responsible for passing the resting order's price as `price`.
    static std::optional<Trade> create(TradeId tradeId, OrderId makerOrderId,
                                       OrderId takerOrderId, Side takerSide,
                                       Price price, Quantity quantity,
                                       Sequence sequence);

    TradeId id() const noexcept { return tradeId_; }
    OrderId makerOrderId() const noexcept { return makerOrderId_; }
    OrderId takerOrderId() const noexcept { return takerOrderId_; }
    Side takerSide() const noexcept { return takerSide_; }
    Price price() const noexcept { return price_; }
    Quantity quantity() const noexcept { return quantity_; }
    Sequence sequence() const noexcept { return sequence_; }

    OrderId buyOrderId() const noexcept {
        return takerSide_ == Side::Buy ? takerOrderId_ : makerOrderId_;
    }
    OrderId sellOrderId() const noexcept {
        return takerSide_ == Side::Buy ? makerOrderId_ : takerOrderId_;
    }

private:
    Trade(TradeId tradeId, OrderId makerOrderId, OrderId takerOrderId, Side takerSide,
          Price price, Quantity quantity, Sequence sequence) noexcept;

    TradeId tradeId_;
    OrderId makerOrderId_;
    OrderId takerOrderId_;
    Side takerSide_;
    Price price_;
    Quantity quantity_;
    Sequence sequence_;
};

}  // namespace lob
