// Integration tests: the real Cli driving a real OrderManager through in-memory
// streams. Assertions are on facts (manager state, key words, numbers, relative
// ordering), not on exact column widths, so cosmetic formatting can change.
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>

#include "Cli.h"
#include "OrderManager.h"
#include "TestFramework.h"

using namespace lob;

namespace {

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Position of `part` in `text`, or npos.
std::size_t at(const std::string& text, const std::string& part) { return text.find(part); }

std::size_t count(const std::string& text, const std::string& part) {
    std::size_t n = 0;
    for (std::size_t p = text.find(part); p != std::string::npos; p = text.find(part, p + part.size())) ++n;
    return n;
}

struct Harness {
    OrderManager manager;
    std::ostringstream out;
    std::ostringstream err;
    Cli cli{manager, out, err, Cli::Options{false}};

    // Runs one line; returns what it printed to `out`. The error stream is in `err`.
    std::string run(const std::string& line) {
        out.str("");
        err.str("");
        lastStatus = cli.handleLine(line);
        return out.str();
    }
    Cli::Status lastStatus = Cli::Status::Continue;
    std::string errors() const { return err.str(); }
};

bool consistent(const OrderManager& m) {
    const InvariantReport r = m.checkConsistency();
    for (const auto& v : r.violations) std::cerr << "    consistency violated: " << v << "\n";
    return r.ok() && m.book().checkInvariants().ok();
}

}  // namespace

// ---- submit ------------------------------------------------------------------------------------------------

TEST(SubmitWithGeneratedIdShowsTheAssignedId) {
    Harness h;
    const std::string o = h.run("submit BUY 100 50");
    CHECK(has(o, "Order 1: ACCEPTED"));
    CHECK(has(o, "generated id"));
    CHECK(has(o, "NEW"));
    CHECK(has(o, "remaining: 50"));
    CHECK(has(o, "trades:    none"));
    CHECK(h.errors().empty());
    CHECK(h.manager.knows(1));
    CHECK_EQ(h.manager.order(1)->remainingQuantity, 50);
    CHECK(consistent(h.manager));
}

TEST(SubmitWithClientIdUsesThatId) {
    Harness h;
    const std::string o = h.run("submit BUY 99 100 1001");
    CHECK(has(o, "Order 1001: ACCEPTED"));
    CHECK(has(o, "client id"));
    CHECK(h.manager.knows(1001));
    // Generated ids still start at 1: the CLI invents no id mechanism of its own.
    CHECK(has(h.run("submit SELL 150 1"), "Order 1: ACCEPTED"));
}

TEST(SubmitDuplicateIdIsABusinessRejectionNotAnError) {
    Harness h;
    h.run("submit BUY 100 50 7");
    const std::string o = h.run("submit SELL 90 10 7");  // would trade if accepted
    CHECK(has(o, "REJECTED"));
    CHECK(has(o, "duplicate order id"));
    CHECK(h.errors().empty());  // reported as a result, on the normal stream
    CHECK_EQ(h.manager.trades().size(), 0u);
    CHECK_EQ(h.manager.activeOrderCount(), 1u);
    CHECK(consistent(h.manager));
}

TEST(SubmitCrossingShowsTradesAndFinalState) {
    Harness h;
    h.run("submit SELL 105 20");
    h.run("submit SELL 106 20");
    const std::string o = h.run("submit BUY 106 30");
    CHECK(has(o, "Order 3: ACCEPTED"));
    CHECK(has(o, "FILLED"));
    CHECK(has(o, "executed:  30"));
    CHECK(has(o, "remaining: 0"));
    CHECK(has(o, "trades:    2"));
    CHECK(has(o, "105"));  // executed at the resting prices
    CHECK(has(o, "106"));
    CHECK_EQ(h.manager.trades().size(), 2u);
    CHECK_EQ(h.manager.trades()[0].price(), 105);
    CHECK_EQ(h.manager.trades()[1].price(), 106);
    CHECK_EQ(h.manager.trades()[1].quantity(), 10);
    CHECK(consistent(h.manager));
}

TEST(SubmitPartiallyMatchedShowsRestingRemainder) {
    Harness h;
    h.run("submit SELL 100 10");
    const std::string o = h.run("submit BUY 100 25");
    CHECK(has(o, "PARTIALLY_FILLED"));
    CHECK(has(o, "executed:  10"));
    CHECK(has(o, "remaining: 15"));
    CHECK(has(o, "resting"));
    CHECK(consistent(h.manager));
}

TEST(InvalidSubmitInputNeverReachesTheManager) {
    Harness h;
    for (const char* line : {"submit", "submit BUY", "submit BUY 100", "submit HOLD 100 5", "submit BUY x 5",
                             "submit BUY 100 y", "submit BUY 0 5", "submit BUY -1 5", "submit BUY 100 0",
                             "submit BUY 100 -5", "submit BUY 10.5 5", "submit BUY 100 5 0",
                             "submit BUY 100 5 abc", "submit BUY 100 5 1 2",
                             "submit BUY 9223372036854775808 5"}) {
        const std::string o = h.run(line);
        CHECK(o.empty());
        CHECK(has(h.errors(), "error:"));
        CHECK(h.lastStatus == Cli::Status::Continue);
    }
    CHECK_EQ(h.manager.recordedOrderCount(), 0u);  // not even a rejected record: no id was burned
    CHECK(has(h.run("submit BUY 100 5"), "Order 1: ACCEPTED"));  // and the CLI still works
}

// ---- cancel -----------------------------------------------------------------------------------------------------

TEST(CancelAnUnfilledOrder) {
    Harness h;
    h.run("submit BUY 100 50");
    const std::string o = h.run("cancel 1");
    CHECK(has(o, "Order 1: CANCELLED"));
    CHECK(has(o, "executed (kept): 0"));
    CHECK(has(o, "cancelled:       50"));
    CHECK(h.manager.book().empty());
    CHECK(h.manager.order(1)->status == OrderStatus::Cancelled);
    CHECK(consistent(h.manager));
}

TEST(CancelAPartiallyFilledOrderKeepsItsTrades) {
    Harness h;
    h.run("submit BUY 100 100");
    h.run("submit SELL 100 40");
    const std::string o = h.run("cancel 1");
    CHECK(has(o, "executed (kept): 40"));
    CHECK(has(o, "cancelled:       60"));
    CHECK_EQ(h.manager.tradesFor(1).size(), 1u);
    CHECK(has(h.run("trades 1"), "40"));  // the execution is still on record
    CHECK(consistent(h.manager));
}

TEST(CancelFailuresAreReportedAndLeaveStateAlone) {
    Harness h;
    h.run("submit SELL 100 10");
    h.run("submit BUY 100 10");   // 1 and 2 are filled
    h.run("submit BUY 90 10");    // 3 rests
    h.run("cancel 3");

    const std::string unknown = h.run("cancel 99");
    CHECK(has(unknown, "REJECTED"));
    CHECK(has(unknown, "order not found"));
    const std::string filled = h.run("cancel 1");
    CHECK(has(filled, "order is not active"));
    CHECK(has(filled, "FILLED"));
    const std::string again = h.run("cancel 3");
    CHECK(has(again, "order is not active"));
    CHECK(has(again, "CANCELLED"));
    CHECK(h.errors().empty());
    CHECK(h.manager.order(1)->status == OrderStatus::Filled);
    CHECK(consistent(h.manager));
}

// ---- modify -------------------------------------------------------------------------------------------------------

TEST(ModifyPriceResetsPriority) {
    Harness h;
    h.run("submit BUY 100 10");
    h.run("submit BUY 101 10");
    const Sequence before = h.manager.order(1)->sequence;
    const std::string o = h.run("modify 1 101 10");
    CHECK(has(o, "MODIFIED"));
    CHECK(has(o, "before:    price 100"));
    CHECK(has(o, "requested: price 101"));
    CHECK(has(o, "RESET"));
    CHECK(h.manager.order(1)->sequence > before);
    CHECK(consistent(h.manager));
}

TEST(ModifyQuantityIncreaseResetsAndDecreasePreservesPriority) {
    Harness h;
    h.run("submit BUY 100 10");
    h.run("submit BUY 100 10");
    CHECK(has(h.run("modify 1 100 25"), "RESET"));      // increase
    CHECK(has(h.run("modify 2 100 4"), "PRESERVED"));   // decrease keeps its place
    CHECK_EQ(h.manager.order(2)->remainingQuantity, 4);
    CHECK(consistent(h.manager));
}

TEST(ModifyNoChangeIsRejectedAndZeroQuantityPointsToCancel) {
    Harness h;
    h.run("submit BUY 100 10");
    const std::string same = h.run("modify 1 100 10");
    CHECK(has(same, "REJECTED"));
    CHECK(has(same, "changes nothing"));
    CHECK(has(same, "unchanged"));
    const std::string zero = h.run("modify 1 100 0");  // syntax-level rejection
    CHECK(zero.empty());
    CHECK(has(h.errors(), "cancel"));
    CHECK_EQ(h.manager.order(1)->remainingQuantity, 10);
    CHECK(consistent(h.manager));
}

TEST(ModifyUnknownAndInactiveOrders) {
    Harness h;
    h.run("submit BUY 100 10");
    h.run("cancel 1");
    CHECK(has(h.run("modify 99 100 5"), "order not found"));
    const std::string o = h.run("modify 1 101 5");
    CHECK(has(o, "order is not active"));
    CHECK(has(o, "CANCELLED"));
    CHECK(consistent(h.manager));
}

TEST(ModifyIntoTheBookShowsTheTrades) {
    Harness h;
    h.run("submit SELL 105 10");
    h.run("submit BUY 100 10");
    const std::string o = h.run("modify 2 105 10");
    CHECK(has(o, "MODIFIED"));
    CHECK(has(o, "FILLED"));
    CHECK(has(o, "trades:    1"));
    CHECK(has(o, "not applicable"));  // priority is moot once it filled
    CHECK_EQ(h.manager.trades().size(), 1u);
    CHECK(consistent(h.manager));
}

TEST(ModifyKeepsExecutedQuantity) {
    Harness h;
    h.run("submit BUY 100 100");
    h.run("submit SELL 100 40");
    const std::string o = h.run("modify 1 100 30");  // remaining 60 -> 30
    CHECK(has(o, "executed 40"));
    CHECK(has(o, "remaining 30"));
    CHECK(has(o, "PRESERVED"));
    const auto rec = h.manager.order(1);
    CHECK_EQ(rec->executedQuantity, 40);
    CHECK_EQ(rec->effectiveQuantity, 70);
    CHECK(consistent(h.manager));
}

// ---- book / depth / best -------------------------------------------------------------------------------------------------

TEST(EmptyBookAndEmptyMarketData) {
    Harness h;
    const std::string book = h.run("book");
    CHECK(has(book, "ASKS"));
    CHECK(has(book, "BIDS"));
    CHECK_EQ(count(book, "(none)"), 2u);
    CHECK(has(book, "Best bid:  -"));
    CHECK(has(book, "Best ask:  -"));
    CHECK(has(book, "Spread:    -"));
    CHECK(has(book, "Mid-price: -"));
    const std::string best = h.run("best");
    CHECK(has(best, "Best bid:  -"));
    CHECK(has(h.run("depth 3"), "(none)"));
    CHECK(h.errors().empty());
}

TEST(PopulatedBookIsOrderedAndSummarised) {
    Harness h;
    h.run("submit SELL 105 30");
    h.run("submit SELL 103 10");
    h.run("submit SELL 103 5");
    h.run("submit BUY 99 7");
    h.run("submit BUY 100 20");
    h.run("submit BUY 98 1");
    const std::string o = h.run("book");

    // Asks lowest first, bids highest first.
    CHECK(at(o, "103") < at(o, "105"));
    const std::size_t bidsAt = at(o, "BIDS");
    CHECK(bidsAt != std::string::npos);
    const std::string bids = o.substr(bidsAt);
    CHECK(at(bids, "100") < at(bids, "99"));
    CHECK(at(bids, "99") < at(bids, "98"));
    // Aggregates and order counts: 103 holds 10 + 5 = 15 units in 2 orders.
    CHECK(has(o, "Best bid:  100 ticks (20 units in 1 order)"));
    CHECK(has(o, "Best ask:  103 ticks (15 units in 2 orders)"));
    CHECK(has(o, "Spread:    3 ticks"));
    CHECK(has(o, "Mid-price: 101.5 ticks"));  // odd difference: exact half tick, no float
    CHECK(consistent(h.manager));
}

TEST(DepthLimitsLevelsAndHandlesAOneSidedBook) {
    Harness h;
    for (int i = 0; i < 4; ++i) h.run("submit BUY " + std::to_string(100 - i) + " 5");
    const std::string two = h.run("depth 2");
    CHECK(has(two, "100"));
    CHECK(has(two, "99"));
    CHECK(!has(two, " 98 "));
    CHECK(has(two, "more levels exist"));
    CHECK(has(two, "(none)"));  // the empty ask side
    const std::string all = h.run("depth 10");
    CHECK(has(all, "97"));
    CHECK(!has(all, "more levels exist"));
}

TEST(DepthRejectsZeroAndExcessiveValuesSafely) {
    Harness h;
    h.run("submit BUY 100 5");
    for (const char* line : {"depth 0", "depth 1001", "depth 99999999999999999999", "depth -1", "depth x"}) {
        CHECK(h.run(line).empty());
        CHECK(has(h.errors(), "error:"));
    }
    CHECK(has(h.run("depth 1000"), "100"));  // the maximum is accepted
}

TEST(BestShowsHalfTickMidpoint) {
    Harness h;
    h.run("submit BUY 100 1");
    h.run("submit SELL 105 1");
    const std::string o = h.run("best");
    CHECK(has(o, "Mid-price: 102.5 ticks"));
    CHECK(has(o, "Spread:    5 ticks"));
}

// ---- order / trades -------------------------------------------------------------------------------------------------------

TEST(OrderShowsTheLifecycleRecordForRestingFilledCancelledAndRejectedOrders) {
    Harness h;
    h.run("submit BUY 100 50");        // 1: will be partially filled, then cancelled
    h.run("submit SELL 100 20");       // 2: filled
    h.run("cancel 1");
    h.manager.submit(9, Side::Buy, 0, 10);  // 9: a rejected record, made directly (the CLI refuses such input)

    const std::string cancelled = h.run("order 1");
    CHECK(has(cancelled, "CANCELLED"));
    CHECK(has(cancelled, "executed:          20"));
    CHECK(has(cancelled, "cancelled:         30"));
    CHECK(has(cancelled, "cancelled because"));
    CHECK(has(cancelled, "submitted 50, effective 50"));

    const std::string filled = h.run("order 2");
    CHECK(has(filled, "FILLED"));
    CHECK(has(filled, "SELL"));
    CHECK(has(filled, "remaining:         0"));

    const std::string rejected = h.run("order 9");
    CHECK(has(rejected, "REJECTED"));
    CHECK(has(rejected, "rejected because"));
    CHECK(has(rejected, "invalid price"));

    CHECK(has(h.run("order 12345"), "not found"));
    CHECK(h.errors().empty());
}

TEST(OrderShowsCurrentAndOriginalPriceAfterAModify) {
    Harness h;
    h.run("submit BUY 100 10");
    h.run("modify 1 103 10");
    const std::string o = h.run("order 1");
    CHECK(has(o, "103 ticks (original 100)"));
}

TEST(TradesAreShownInExecutionOrderForBuyerAndSeller) {
    Harness h;
    h.run("submit SELL 101 5");   // 1
    h.run("submit SELL 102 5");   // 2
    h.run("submit BUY 102 8");    // 3: two trades
    const std::string buyer = h.run("trades 3");
    CHECK(has(buyer, "Trades for order 3"));
    CHECK(at(buyer, "101") < at(buyer, "102"));  // execution order preserved
    CHECK(h.manager.tradesFor(3)[0].takerSide() == Side::Buy);  // the incoming order was the taker
    CHECK(has(buyer, "TAKER"));
    const std::string seller = h.run("trades 2");
    CHECK(has(seller, "102"));
    CHECK(has(h.run("trades 1"), "101"));
    CHECK_EQ(h.manager.tradesFor(3).size(), 2u);
    CHECK(has(h.run("trades 99"), "not found"));
    CHECK(consistent(h.manager));
}

TEST(TradesForAnOrderWithoutTradesAndAfterFullFill) {
    Harness h;
    h.run("submit BUY 90 5");
    CHECK(has(h.run("trades 1"), "(none)"));
    h.run("submit SELL 90 5");
    const std::string after = h.run("trades 1");
    CHECK(!has(after, "(none)"));
    CHECK(has(after, "90"));
}

// ---- help, exit, robustness ------------------------------------------------------------------------------------------------------

TEST(HelpListsEveryCommandAndTheUnits) {
    Harness h;
    const std::string o = h.run("help");
    for (const char* word : {"submit", "cancel", "modify", "book", "depth", "best", "order", "trades",
                             "recover", "help", "exit", "quit"}) {
        CHECK(has(o, word));
    }
    CHECK(has(o, "TICKS"));
    CHECK(has(o, "UNITS"));
    CHECK(has(o, "submit BUY 100 50"));  // examples
}

TEST(ExitAndQuitEndTheSessionInAnyCase) {
    for (const char* word : {"exit", "quit", "EXIT", "Quit", "  exit  "}) {
        Harness h;
        h.run(word);
        CHECK(h.lastStatus == Cli::Status::Exit);
    }
    Harness h;
    h.run("book");
    CHECK(h.lastStatus == Cli::Status::Continue);
}

TEST(MalformedAndRejectedCommandsNeverEndTheSession) {
    Harness h;
    for (const char* line : {"", "   ", "nonsense", "submit", "cancel x", "modify 1", "depth 0", "book extra",
                             "submit BUY 100 5 5 5", "cancel 424242", "modify 424242 5 5", "order 424242",
                             "trades 424242", "\t\t", "SUBMIT"}) {
        h.run(line);
        CHECK(h.lastStatus == Cli::Status::Continue);
    }
    CHECK(has(h.run("submit BUY 100 5"), "ACCEPTED"));  // still fully usable afterwards
    CHECK(consistent(h.manager));
}

TEST(OverlongInputIsRejectedWithoutHarm) {
    Harness h;
    CHECK(h.run(std::string(100000, 'x')).empty());
    CHECK(has(h.errors(), "too long"));
    CHECK(h.run("submit BUY 100 5 " + std::string(3000, '1')).empty());
    CHECK_EQ(h.manager.recordedOrderCount(), 0u);
    CHECK(h.lastStatus == Cli::Status::Continue);
}

TEST(MessagesAreLabelledInTicksAndUnits) {
    Harness h;
    const std::string o = h.run("submit BUY 100 50");
    CHECK(has(o, "units"));
    CHECK(has(o, "ticks"));
    CHECK(has(h.run("book"), "PRICE (ticks)"));
    CHECK(has(h.run("book"), "QUANTITY (units)"));
}

// ---- The run loop -------------------------------------------------------------------------------------------------------------------

TEST(RunProcessesALineScriptAndStopsAtExit) {
    OrderManager manager;
    std::ostringstream out, err;
    Cli cli(manager, out, err, Cli::Options{false});
    std::istringstream in("submit BUY 100 10\n\nsubmit SELL 100 4\nbogus\nexit\nsubmit BUY 1 1\n");
    CHECK_EQ(cli.run(in), 0);
    CHECK_EQ(manager.recordedOrderCount(), 2u);  // nothing after `exit` was executed
    CHECK_EQ(manager.trades().size(), 1u);
    CHECK(has(err.str(), "unknown command"));
    CHECK(consistent(manager));
}

TEST(EndOfInputExitsCleanly) {
    OrderManager manager;
    std::ostringstream out, err;
    Cli cli(manager, out, err, Cli::Options{false});
    std::istringstream in("submit BUY 100 10\nbook");  // no trailing newline, no exit
    CHECK_EQ(cli.run(in), 0);
    CHECK_EQ(manager.recordedOrderCount(), 1u);
    std::istringstream empty("");
    CHECK_EQ(cli.run(empty), 0);
}

TEST(WindowsLineEndingsAreAccepted) {
    OrderManager manager;
    std::ostringstream out, err;
    Cli cli(manager, out, err, Cli::Options{false});
    std::istringstream in("submit BUY 100 10\r\nsubmit SELL 100 10\r\nexit\r\n");
    CHECK_EQ(cli.run(in), 0);
    CHECK_EQ(manager.trades().size(), 1u);
    CHECK(err.str().empty());
}

TEST(PromptAndBannerAppearOnlyWhenEnabled) {
    OrderManager manager;
    std::ostringstream quietOut, quietErr, promptOut, promptErr;
    Cli quiet(manager, quietOut, quietErr, Cli::Options{false});
    Cli chatty(manager, promptOut, promptErr, Cli::Options{true});
    std::istringstream a("help\n"), b("help\n");
    quiet.run(a);
    chatty.run(b);
    CHECK(!has(quietOut.str(), "lob> "));
    CHECK(has(promptOut.str(), "lob> "));
    CHECK(has(promptOut.str(), "Type 'help'"));
}

// ---- Recovery command on a healthy manager; the CLI holds no state of its own ---------------------------------------------------------------

TEST(RecoverOnCurrentHistoryIsAHarmlessNoOp) {
    Harness h;
    h.run("submit SELL 100 10");
    h.run("submit BUY 100 10");
    const std::size_t trades = h.manager.trades().size();
    const std::string o = h.run("recover");
    CHECK(has(o, "already current"));
    CHECK_EQ(h.manager.trades().size(), trades);
    CHECK(has(h.run("recover"), "already current"));  // repeatable
    CHECK(consistent(h.manager));
}

TEST(OrderAndTradesCommandsShowNoStaleWarningWhenHistoryIsCurrent) {
    Harness h;
    h.run("submit BUY 100 5");
    CHECK(!has(h.run("order 1"), "WARNING"));
    CHECK(!has(h.run("trades 1"), "WARNING"));
}

TEST(TheCliReflectsTheManagerNotItsOwnState) {
    Harness h;
    // Orders made behind the CLI's back (directly through the manager) are visible at once.
    h.manager.submit(77, Side::Sell, 120, 8);
    CHECK(has(h.run("book"), "120"));
    CHECK(has(h.run("order 77"), "SELL"));
    h.manager.cancel(77);
    CHECK(has(h.run("order 77"), "CANCELLED"));
    CHECK(has(h.run("book"), "Best ask:  -"));
}

TEST(MixedScriptKeepsEverythingConsistent) {
    Harness h;
    for (const char* line :
         {"submit SELL 105 30", "submit SELL 103 10", "submit BUY 99 7", "submit BUY 104 12", "modify 1 103 30",
          "submit BUY 103 40", "cancel 3", "modify 5 99 5", "submit SELL 98 100", "cancel 1", "order 1",
          "trades 6", "book", "depth 2", "best"}) {
        h.run(line);
        CHECK(consistent(h.manager));
    }
}

TEST_MAIN()
