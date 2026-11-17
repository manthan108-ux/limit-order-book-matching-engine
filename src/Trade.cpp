#include "Trade.h"

namespace lob {

bool Trade::isValid(TradeId tradeId, OrderId makerOrderId, OrderId takerOrderId,
                    Price price, Quantity quantity) noexcept {
    return tradeId != 0 && makerOrderId != 0 && takerOrderId != 0 &&
           makerOrderId != takerOrderId && isValidPrice(price) &&
           isValidQuantity(quantity);
}

std::optional<Trade> Trade::create(TradeId tradeId, OrderId makerOrderId,
                                   OrderId takerOrderId, Side takerSide, Price price,
                                   Quantity quantity, Sequence sequence) {
    if (!isValid(tradeId, makerOrderId, takerOrderId, price, quantity)) {
        return std::nullopt;
    }
    return Trade(tradeId, makerOrderId, takerOrderId, takerSide, price, quantity,
                 sequence);
}

Trade::Trade(TradeId tradeId, OrderId makerOrderId, OrderId takerOrderId,
             Side takerSide, Price price, Quantity quantity, Sequence sequence) noexcept
    : tradeId_(tradeId),
      makerOrderId_(makerOrderId),
      takerOrderId_(takerOrderId),
      takerSide_(takerSide),
      price_(price),
      quantity_(quantity),
      sequence_(sequence) {}

}  // namespace lob
