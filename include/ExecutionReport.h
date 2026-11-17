#pragma once

#include <cstddef>
#include <vector>

#include "OrderTypes.h"
#include "Trade.h"

namespace lob {

// Outcome of one operation on an order. A plain value type with no I/O, so the
// order manager, the CLI or an API can all consume it.
//
// Reading a report for an ADD:
//   accepted == false                       rejected; `rejectReason` says why,
//                                           status is REJECTED, nothing changed.
//   accepted, status NEW, no trades         rested in the book untouched.
//   accepted, status PARTIALLY_FILLED       traded, and the remainder rests.
//   accepted, status FILLED                 fully executed, nothing rests.
// "Accepted with zero trades" is therefore distinguishable from "rejected".
//
// `remainingQuantity` is the quantity still working in the book after the
// operation (0 for FILLED, CANCELLED and REJECTED orders). The final state is
// carried by `status` alone; there is no second, redundant state flag.
//
// The same type reports CANCEL and MODIFY (see `operation`). For those, and
// for a failed operation on a known order, `status` is the order's current
// status and `originalQuantity` its effective total (executed + remaining, or
// + cancelled quantity once cancelled); for an unknown order `status` is only
// the placeholder REJECTED.
struct ExecutionReport {
    Operation operation = Operation::Add;
    OrderId orderId = 0;
    bool accepted = false;
    RejectReason rejectReason = RejectReason::None;
    CancelReason cancelReason = CancelReason::None;  // set on a successful cancel
    OrderStatus status = OrderStatus::Rejected;
    Quantity originalQuantity = 0;
    Quantity executedQuantity = 0;
    Quantity remainingQuantity = 0;
    std::vector<Trade> trades;  // in execution order; this order is always the taker

    std::size_t tradeCount() const noexcept { return trades.size(); }
    bool rested() const noexcept { return accepted && remainingQuantity > 0; }
};

}  // namespace lob
