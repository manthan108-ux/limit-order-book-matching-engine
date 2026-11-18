#include "Cli.h"

#include <exception>
#include <iomanip>
#include <istream>
#include <limits>
#include <new>
#include <ostream>
#include <string>
#include <vector>

namespace lob {

namespace {

// ---- Small formatting helpers -------------------------------------------------------

std::string noun(std::size_t n, const char* singular, const char* plural) {
    return std::to_string(n) + " " + (n == 1 ? singular : plural);
}

std::string midpointText(const Midpoint& m) {
    return std::to_string(m.ticks) + (m.halfTick ? ".5" : "");
}

// "price x quantity" cells are right-aligned numbers under named headers.
void printLevels(std::ostream& out, const char* title, const std::vector<LevelSummary>& levels) {
    out << title << "\n";
    if (levels.empty()) {
        out << "  (none)\n";
        return;
    }
    out << "  " << std::setw(14) << "PRICE (ticks)" << std::setw(18) << "QUANTITY (units)"
        << std::setw(9) << "ORDERS" << "\n";
    for (const LevelSummary& l : levels) {
        out << "  " << std::setw(14) << l.price << std::setw(18) << l.totalQuantity << std::setw(9)
            << l.orderCount << "\n";
    }
}

void printTradeRow(std::ostream& out, const Trade& t) {
    out << "  " << std::setw(9) << t.id() << std::setw(15) << t.price() << std::setw(13)
        << t.quantity() << std::setw(10) << t.buyOrderId() << std::setw(10) << t.sellOrderId()
        << std::setw(8) << toString(t.takerSide()) << std::setw(10) << t.sequence() << "\n";
}

void printTradeHeader(std::ostream& out) {
    out << "  " << std::setw(9) << "TRADE ID" << std::setw(15) << "PRICE (ticks)" << std::setw(13)
        << "QTY (units)" << std::setw(10) << "BUYER" << std::setw(10) << "SELLER" << std::setw(8)
        << "TAKER" << std::setw(10) << "SEQ" << "\n";
}

void printTrades(std::ostream& out, const std::vector<Trade>& trades) {
    printTradeHeader(out);
    for (const Trade& t : trades) printTradeRow(out, t);
}

void printSide(std::ostream& out, const char* label, const std::optional<LevelSummary>& best) {
    out << "  " << std::left << std::setw(11) << label << std::right;
    if (!best) {
        out << "-\n";
        return;
    }
    out << best->price << " ticks (" << noun(static_cast<std::size_t>(best->totalQuantity), "unit", "units")
        << " in " << noun(best->orderCount, "order", "orders") << ")\n";
}

void printMarketSummary(std::ostream& out, const OrderBook& book) {
    out << "MARKET\n";
    printSide(out, "Best bid:", book.bestBid());
    printSide(out, "Best ask:", book.bestAsk());
    out << "  " << std::left << std::setw(11) << "Spread:" << std::right;
    if (const auto spread = book.spread()) {
        out << *spread << " ticks\n";
    } else {
        out << "-\n";
    }
    out << "  " << std::left << std::setw(11) << "Mid-price:" << std::right;
    if (const auto mid = book.midPrice()) {
        out << midpointText(*mid) << " ticks\n";
    } else {
        out << "-\n";
    }
}

const char* const kHelpText =
    "Commands (case-insensitive; prices are integer TICKS, quantities are integer UNITS):\n"
    "  submit <BUY|SELL> <PRICE> <QUANTITY> [ID]\n"
    "                       Enter a limit order. Without ID one is generated.\n"
    "                       e.g.  submit BUY 100 50     submit SELL 105 30     submit BUY 99 100 1001\n"
    "  cancel <ORDER_ID>    Cancel the remaining quantity of an active order.   e.g.  cancel 1001\n"
    "  modify <ORDER_ID> <NEW_PRICE> <NEW_REMAINING_QUANTITY>\n"
    "                       Change price and/or remaining quantity.   e.g.  modify 1001 102 40\n"
    "                       A price change or a quantity increase loses time priority;\n"
    "                       a quantity decrease keeps it. No change is rejected (NoChange).\n"
    "                       Quantity 0 is rejected: use 'cancel'.\n"
    "  book                 Show every price level on both sides, plus best prices and spread.\n"
    "  depth <LEVELS>       Show the top LEVELS (1-1000) price levels on each side.   e.g.  depth 5\n"
    "  best                 Show the best bid, best ask, spread and mid-price.\n"
    "  order <ORDER_ID>     Show an order's lifecycle record (also for filled/cancelled/rejected).\n"
    "  trades <ORDER_ID>    Show the trades an order took part in, in execution order.\n"
    "  recover              Finish bookkeeping left pending by an out-of-memory failure.\n"
    "                       Safe to repeat. It never resubmits or re-executes anything.\n"
    "  help                 Show this text.\n"
    "  exit | quit          Leave the program.\n"
    "Notes: price and quantity are whole numbers (no decimals, no rounding). One tick is\n"
    "the smallest price step; no currency value is attached to it. Trade 'SEQ' is the\n"
    "arrival sequence of the incoming (taker) order that caused the trade.\n";

}  // namespace

// ---- Construction and the line loop -----------------------------------------------------

Cli::Cli(OrderManager& manager, std::ostream& out, std::ostream& err, Options options)
    : manager_(manager), out_(out), err_(err), options_(options) {}

Cli::Status Cli::handleLine(std::string_view line) {
    try {
        const ParseResult parsed = parseCommand(line);
        if (!parsed.ok()) {
            err_ << "error: " << parsed.error << "\n";
            return Status::Continue;
        }
        return execute(parsed.command);
    } catch (const std::exception& e) {
        reportFailure(CommandType::Empty, e.what());
    } catch (...) {
        reportFailure(CommandType::Empty, "unknown failure");
    }
    return Status::Continue;
}

Cli::Status Cli::execute(const ParsedCommand& command) {
    try {
        return dispatch(command);
    } catch (const std::bad_alloc&) {
        reportFailure(command.type, "out of memory");
    } catch (const std::exception& e) {
        reportFailure(command.type, e.what());
    } catch (...) {
        reportFailure(command.type, "unknown failure");
    }
    return Status::Continue;
}

int Cli::run(std::istream& in) {
    if (options_.showPrompt) {
        out_ << "Limit Order Book matching engine. Prices are integer ticks, quantities integer units.\n"
                "Type 'help' for the commands, 'exit' to leave.\n";
    }
    std::string line;
    while (true) {
        if (options_.showPrompt) {
            out_ << "lob> " << std::flush;
        }
        if (!std::getline(in, line)) {  // end of input: a normal way to finish
            if (options_.showPrompt) out_ << "\n";
            break;
        }
        if (handleLine(line) == Status::Exit) break;
    }
    out_ << std::flush;
    return 0;
}

Cli::Status Cli::dispatch(const ParsedCommand& c) {
    switch (c.type) {
        case CommandType::Empty:   break;
        case CommandType::Submit:  doSubmit(c); break;
        case CommandType::Cancel:  doCancel(*c.orderId); break;
        case CommandType::Modify:  doModify(c); break;
        case CommandType::Book:    doBook(); break;
        case CommandType::Depth:   doDepth(c.levels); break;
        case CommandType::Order:   doOrder(*c.orderId); break;
        case CommandType::Trades:  doTrades(*c.orderId); break;
        case CommandType::Best:    doBest(); break;
        case CommandType::Recover: doRecover(); break;
        case CommandType::Help:    doHelp(); break;
        case CommandType::Exit:    return Status::Exit;
    }
    return Status::Continue;
}

// ---- Failure and staleness messages ---------------------------------------------------------
// These print string literals and integers only, so they still work when memory is exhausted.

void Cli::reportFailure(CommandType type, const char* what) noexcept {
    try {
        err_ << "error: the command failed (" << what << ").\n";
        const bool trading = type == CommandType::Submit || type == CommandType::Cancel ||
                             type == CommandType::Modify;
        if (trading) {
            err_ << "  The operation may already have executed, in whole or in part.\n"
                    "  Do NOT resubmit it. Run 'recover', then check 'book' and 'order <ID>'.\n";
        }
        if (!manager_.isHistoryCurrent()) {
            err_ << "  Bookkeeping recovery is pending: run 'recover'.\n";
        }
    } catch (...) {
        // Nothing more can be done if even reporting fails.
    }
}

void Cli::printStaleWarning() {
    out_ << "WARNING: bookkeeping recovery is pending. History shown below may be out of date.\n"
            "         Run 'recover' (it is safe to repeat) before relying on it.\n";
}

void Cli::printPendingNote() {
    if (!manager_.isHistoryCurrent()) {
        out_ << "note: the operation completed, but bookkeeping recovery is pending.\n"
                "      Order history may lag until you run 'recover'.\n";
    }
}

// ---- Trading commands ---------------------------------------------------------------------

void Cli::doSubmit(const ParsedCommand& c) {
    const ExecutionReport r = c.orderId ? manager_.submit(*c.orderId, c.side, c.price, c.quantity)
                                        : manager_.submit(c.side, c.price, c.quantity);
    if (!r.accepted) {
        if (r.orderId != 0) {
            out_ << "Order " << r.orderId << ": REJECTED (" << toString(r.rejectReason) << ")\n";
            // A usable id with a rejected submission is recorded and retired by the manager.
            if (manager_.knows(r.orderId)) out_ << "  The id is now retired and cannot be reused.\n";
        } else {
            out_ << "Submission REJECTED (" << toString(r.rejectReason) << ")\n";
        }
        printPendingNote();
        return;
    }
    out_ << "Order " << r.orderId << ": ACCEPTED  " << toString(c.side) << " " << c.quantity
         << " units @ " << c.price << " ticks"
         << (c.orderId ? "  (client id)" : "  (generated id)") << "\n";
    out_ << "  status:    " << toString(r.status) << "\n";
    out_ << "  executed:  " << r.executedQuantity << " units\n";
    out_ << "  remaining: " << r.remainingQuantity << " units"
         << (r.remainingQuantity > 0 ? " (resting in the book)" : "") << "\n";
    if (r.trades.empty()) {
        out_ << "  trades:    none\n";
    } else {
        out_ << "  trades:    " << r.trades.size() << "\n";
        printTrades(out_, r.trades);
    }
    printPendingNote();
}

void Cli::doCancel(OrderId id) {
    const ExecutionReport r = manager_.cancel(id);
    if (!r.accepted) {
        out_ << "Order " << id << ": cancel REJECTED (" << toString(r.rejectReason) << ")\n";
        if (r.rejectReason != RejectReason::OrderNotFound) {
            out_ << "  current state: " << toString(r.status) << ", executed " << r.executedQuantity
                 << " units, remaining " << r.remainingQuantity << " units\n";
        }
        printPendingNote();
        return;
    }
    out_ << "Order " << id << ": CANCELLED\n";
    out_ << "  executed (kept): " << r.executedQuantity << " units\n";
    out_ << "  cancelled:       " << (r.originalQuantity - r.executedQuantity) << " units\n";
    out_ << "  state:           " << toString(r.status) << "\n";
    out_ << "  reason:          " << toString(r.cancelReason) << "\n";
    printPendingNote();
}

void Cli::doModify(const ParsedCommand& c) {
    const OrderId id = *c.orderId;
    // What the order looked like before, from the lifecycle record (only trusted when
    // the history is current; otherwise the "before" lines say so).
    std::optional<OrderRecord> before;
    if (manager_.isHistoryCurrent()) before = manager_.order(id);

    const ExecutionReport r = manager_.modify(id, c.price, c.quantity);

    if (!r.accepted) {
        out_ << "Order " << id << ": modify REJECTED (" << toString(r.rejectReason) << ")\n";
        if (r.rejectReason != RejectReason::OrderNotFound) {
            out_ << "  current state: " << toString(r.status) << ", remaining " << r.remainingQuantity
                 << " units; the order is unchanged\n";
        }
        printPendingNote();
        return;
    }
    out_ << "Order " << id << ": MODIFIED\n";
    if (before) {
        out_ << "  before:    price " << before->price << " ticks, remaining " << before->remainingQuantity
             << " units\n";
    } else {
        out_ << "  before:    (unavailable while bookkeeping recovery is pending)\n";
    }
    out_ << "  requested: price " << c.price << " ticks, remaining " << c.quantity << " units\n";
    out_ << "  state:     " << toString(r.status) << ", executed " << r.executedQuantity
         << " units, remaining " << r.remainingQuantity << " units\n";

    const std::optional<OrderRecord> after = manager_.isHistoryCurrent() ? manager_.order(id) : std::nullopt;
    if (r.status == OrderStatus::Filled) {
        out_ << "  priority:  not applicable (the order filled completely)\n";
    } else if (before && after) {
        out_ << "  priority:  "
             << (after->sequence == before->sequence
                     ? "PRESERVED (kept its place in the queue)"
                     : "RESET (moved to the back of the queue at its price)")
             << "\n";
    } else {
        out_ << "  priority:  (unavailable while bookkeeping recovery is pending)\n";
    }
    if (!r.trades.empty()) {
        out_ << "  trades:    " << r.trades.size() << "\n";
        printTrades(out_, r.trades);
    }
    printPendingNote();
}

// ---- Market data ------------------------------------------------------------------------------

void Cli::doBook() {
    const OrderBook& book = manager_.book();
    const MarketDepth all = book.depth(std::numeric_limits<std::size_t>::max());
    printLevels(out_, "ASKS (lowest price first)", all.asks);
    printLevels(out_, "BIDS (highest price first)", all.bids);
    printMarketSummary(out_, book);
}

void Cli::doDepth(std::size_t levels) {
    const OrderBook& book = manager_.book();
    const MarketDepth d = book.depth(levels);
    out_ << "Top " << levels << " level(s) per side\n";
    printLevels(out_, "ASKS (lowest price first)", d.asks);
    printLevels(out_, "BIDS (highest price first)", d.bids);
    if (book.askLevelCount() > d.asks.size() || book.bidLevelCount() > d.bids.size()) {
        out_ << "  (more levels exist; use 'book' for all of them)\n";
    }
}

void Cli::doBest() { printMarketSummary(out_, manager_.book()); }

// ---- Lifecycle queries -----------------------------------------------------------------------------

void Cli::doOrder(OrderId id) {
    if (!manager_.isHistoryCurrent()) printStaleWarning();
    const std::optional<OrderRecord> rec = manager_.order(id);
    if (!rec) {
        out_ << "Order " << id << ": not found (no such id has been recorded)\n";
        return;
    }
    out_ << "Order " << rec->id << (manager_.isHistoryCurrent() ? "" : "  (possibly out of date)") << "\n";
    out_ << "  side:              " << toString(rec->side) << "\n";
    out_ << "  status:            " << toString(rec->status) << "\n";
    out_ << "  price:             " << rec->price << " ticks (original " << rec->originalPrice << ")\n";
    out_ << "  quantity (units):  submitted " << rec->submittedQuantity << ", effective "
         << rec->effectiveQuantity << "\n";
    out_ << "  executed:          " << rec->executedQuantity << "\n";
    out_ << "  remaining:         " << rec->remainingQuantity << "\n";
    out_ << "  cancelled:         " << rec->cancelledQuantity << "\n";
    out_ << "  sequence:          " << rec->sequence << " (submitted at " << rec->submissionSequence << ")\n";
    out_ << "  trades:            " << rec->tradeIndices.size() << "\n";
    if (rec->status == OrderStatus::Rejected) {
        out_ << "  rejected because:  " << toString(rec->rejectReason) << "\n";
    }
    if (rec->status == OrderStatus::Cancelled) {
        out_ << "  cancelled because: " << toString(rec->cancelReason) << "\n";
    }
}

void Cli::doTrades(OrderId id) {
    if (!manager_.isHistoryCurrent()) printStaleWarning();
    if (!manager_.knows(id)) {
        out_ << "Order " << id << ": not found (no such id has been recorded)\n";
        return;
    }
    const std::vector<Trade> trades = manager_.tradesFor(id);
    out_ << "Trades for order " << id << " (execution order)"
         << (manager_.isHistoryCurrent() ? "" : "  (possibly out of date)") << "\n";
    if (trades.empty()) {
        out_ << "  (none)\n";
        return;
    }
    printTrades(out_, trades);
}

// ---- Recovery ------------------------------------------------------------------------------------------

void Cli::doRecover() {
    // OrderManager does all the work. Only literals are printed, so this also works
    // while memory is still exhausted. Nothing is resubmitted or re-executed.
    const bool wasCurrent = manager_.isHistoryCurrent();
    if (manager_.recover()) {
        out_ << (wasCurrent ? "History is already current; there is nothing to recover.\n"
                            : "Recovery complete: history is now current.\n");
    } else {
        out_ << "Recovery incomplete: bookkeeping is still pending (memory may still be exhausted).\n"
                "  Nothing was lost or duplicated. Run 'recover' again to retry.\n";
    }
}

void Cli::doHelp() { out_ << kHelpText; }

}  // namespace lob
