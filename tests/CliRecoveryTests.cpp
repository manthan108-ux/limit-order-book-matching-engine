// Phase 5.1 recovery integration for the CLI. Stale bookkeeping can only be produced
// by simulating out-of-memory, so this is its own test executable with the same
// isolated, deterministic operator new replacement as FaultInjectionTests.cpp (see
// FaultInjection.h). The injector is armed only around the call under test; the test
// output streams below are exempt from it so that the CLI can print its messages.
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Cli.h"
#include "FaultInjection.h"
#include "OrderManager.h"
#include "TestFramework.h"

#if defined(_MSC_VER)
#define LOB_TEST_NOINLINE __declspec(noinline)
#else
#define LOB_TEST_NOINLINE __attribute__((noinline))
#endif

// Out of line, as in FaultInjectionTests.cpp (see the comment there).
LOB_TEST_NOINLINE void* operator new(std::size_t size) {
    if (fault::shouldFail()) throw std::bad_alloc();
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
LOB_TEST_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
LOB_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace lob;

namespace {

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A string buffer whose own growth is exempt from the injected failures.
class PausingBuf : public std::stringbuf {
protected:
    int_type overflow(int_type c) override {
        fault::Pause p;
        return std::stringbuf::overflow(c);
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        fault::Pause p;
        return std::stringbuf::xsputn(s, n);
    }
};

struct Rig {
    std::unique_ptr<OrderManager> manager;
    PausingBuf outBuf;
    PausingBuf errBuf;
    std::ostream out{&outBuf};
    std::ostream err{&errBuf};
    Cli cli;

    explicit Rig(std::unique_ptr<OrderManager> m)
        : manager(std::move(m)), cli(*manager, out, err, Cli::Options{false}) {}

    std::string run(const std::string& line) {
        outBuf.str("");
        errBuf.str("");
        cli.handleLine(line);
        return outBuf.str();
    }
    std::string errors() const { return errBuf.str(); }
};

std::unique_ptr<OrderManager> sellers(int count) {
    auto m = std::make_unique<OrderManager>();
    for (int i = 1; i <= count; ++i) m->submit(static_cast<OrderId>(i), S, 100, 10);
    return m;
}

// A manager whose last submit SUCCEEDED but whose history bookkeeping lagged because
// allocation #n failed (see OrderManager "deferred bookkeeping").
std::unique_ptr<OrderManager> staleManager(long n, bool& stale) {
    auto m = sellers(3);  // ids 1..3, 10 units each at 100
    bool threw = false;
    {
        fault::FailAllocation guard(n);
        try {
            m->submit(5, B, 100, 30);  // three trades against 1, 2, 3
        } catch (const std::bad_alloc&) {
            threw = true;
        }
    }
    stale = !threw && !m->isHistoryCurrent();
    return m;
}

std::vector<long> stalePoints() {
    std::vector<long> points;
    for (long n = 1; n <= 80; ++n) {
        bool stale = false;
        auto m = staleManager(n, stale);
        if (stale) points.push_back(n);
    }
    return points;
}

std::size_t recordedTradeIndices(const OrderManager& m) {
    std::size_t total = 0;
    for (OrderId id = 1; id <= 20; ++id) {
        if (const auto rec = m.order(id)) total += rec->tradeIndices.size();
    }
    return total;
}

bool healthy(const OrderManager& m) {
    const InvariantReport r = m.checkConsistency();
    for (const auto& v : r.violations) std::cerr << "    consistency violated: " << v << "\n";
    return r.ok() && m.book().checkInvariants().ok();
}

ParsedCommand parsed(const std::string& line) {
    const ParseResult r = parseCommand(line);
    if (!r.ok()) std::cerr << "    test setup: cannot parse " << line << ": " << r.error << "\n";
    return r.command;
}

}  // namespace

// ---- The scenario generator itself is sound ------------------------------------------------

TEST(StaleHistoryScenariosExist) {
    const std::vector<long> points = stalePoints();
    CHECK(points.size() >= 3);  // several distinct interruption points, incl. part-way ones
    bool sawPartial = false;
    for (long n : points) {
        bool stale = false;
        auto m = staleManager(n, stale);
        CHECK(stale);
        CHECK_EQ(m->trades().size(), 3u);        // the engine really did execute all three trades
        CHECK(m->book().checkInvariants().ok());  // and the book is fine
        const std::size_t applied = recordedTradeIndices(*m) / 2;
        if (applied > 0 && applied < 3) sawPartial = true;
    }
    CHECK(sawPartial);
    std::cout << "    [cli-recovery] " << points.size() << " distinct stale-history states\n";
}

// ---- Stale history is never presented as current ------------------------------------------------

TEST(OrderAndTradesCommandsWarnWhileHistoryIsStale) {
    for (long n : stalePoints()) {
        bool stale = false;
        Rig rig(staleManager(n, stale));
        CHECK(stale);
        for (const char* line : {"order 5", "order 1", "trades 5", "trades 2"}) {
            const std::string o = rig.run(line);
            CHECK(has(o, "WARNING"));
            CHECK(has(o, "recovery is pending"));
            CHECK(has(o, "'recover'"));
            CHECK(has(o, "out of date"));
        }
        // Looking never heals and never executes anything.
        CHECK(!rig.manager->isHistoryCurrent());
        CHECK_EQ(rig.manager->trades().size(), 3u);
        CHECK_EQ(rig.manager->recordedOrderCount(), 4u);
        // The book is the authoritative state and is shown without a history caveat.
        const std::string book = rig.run("book");
        CHECK(!has(book, "WARNING"));
        CHECK(has(book, "(none)"));  // all three sellers and the buyer are gone
    }
}

TEST(SnapshotsShownWhileStaleAreLabelledPossiblyOutOfDate) {
    bool stale = false;
    Rig rig(staleManager(stalePoints().front(), stale));
    CHECK(stale);
    CHECK(has(rig.run("order 5"), "possibly out of date"));
    CHECK(has(rig.run("trades 5"), "possibly out of date"));
}

// ---- The recover command ---------------------------------------------------------------------------

TEST(RecoverCommandCompletesTheHistoryWithoutDuplicates) {
    for (long n : stalePoints()) {
        bool stale = false;
        Rig rig(staleManager(n, stale));
        CHECK(stale);
        const std::string o = rig.run("recover");
        CHECK(has(o, "Recovery complete"));
        CHECK(rig.manager->isHistoryCurrent());

        // Now the history is real, and shown without a warning.
        const std::string order = rig.run("order 5");
        CHECK(!has(order, "WARNING"));
        CHECK(has(order, "FILLED"));
        CHECK(has(order, "executed:          30"));
        const std::string trades = rig.run("trades 5");
        CHECK(!has(trades, "WARNING"));
        CHECK_EQ(rig.manager->tradesFor(5).size(), 3u);
        CHECK_EQ(rig.manager->tradesFor(1).size(), 1u);

        // Exactly once per side: nothing duplicated, nothing lost, nothing re-executed.
        CHECK_EQ(rig.manager->trades().size(), 3u);
        CHECK_EQ(recordedTradeIndices(*rig.manager), 2 * rig.manager->trades().size());
        CHECK_EQ(rig.manager->recordedOrderCount(), 4u);
        CHECK(healthy(*rig.manager));
    }
}

TEST(RecoverIsIdempotent) {
    bool stale = false;
    Rig rig(staleManager(stalePoints().front(), stale));
    CHECK(rig.run("recover").size() > 0);
    CHECK(has(rig.run("recover"), "already current"));
    CHECK(has(rig.run("recover"), "already current"));
    CHECK_EQ(rig.manager->trades().size(), 3u);
    CHECK_EQ(recordedTradeIndices(*rig.manager), 6u);
    CHECK(healthy(*rig.manager));
}

TEST(IncompleteRecoveryIsReportedAndCanBeRetried) {
    for (long n : stalePoints()) {
        bool stale = false;
        Rig rig(staleManager(n, stale));
        CHECK(stale);
        const ParsedCommand recoverCmd = parsed("recover");

        // Attempt 1 with memory still exhausted: must not throw, corrupt or lose anything.
        {
            fault::FailAllocation starved(1, /*persistent=*/true);
            rig.outBuf.str("");
            rig.cli.execute(recoverCmd);
        }
        const std::string first = rig.outBuf.str();
        if (rig.manager->isHistoryCurrent()) {
            CHECK(has(first, "Recovery complete"));   // (a state that needed no allocation)
        } else {
            CHECK(has(first, "Recovery incomplete"));
            CHECK(has(first, "again"));               // the user is told they can retry
            // Still reported as stale by the queries, still no duplicates, nothing re-executed.
            CHECK(has(rig.run("order 5"), "WARNING"));
            CHECK_EQ(rig.manager->trades().size(), 3u);
            CHECK_EQ(rig.manager->recordedOrderCount(), 4u);
            const InvariantReport r = rig.manager->checkConsistency();
            CHECK_EQ(r.violations.size(), 1u);        // only the "not current" notice
        }

        // Attempt 2, still starved: same story, and still safe to repeat.
        {
            fault::FailAllocation starved(1, true);
            rig.cli.execute(recoverCmd);
        }
        CHECK_EQ(rig.manager->trades().size(), 3u);
        CHECK(recordedTradeIndices(*rig.manager) <= 6u);  // never MORE than one per side

        // Attempt 3, memory is back: completes, and the data is right.
        CHECK(has(rig.run("recover"), "Recovery complete") || has(rig.run("recover"), "already current"));
        CHECK(rig.manager->isHistoryCurrent());
        CHECK_EQ(recordedTradeIndices(*rig.manager), 6u);
        CHECK(has(rig.run("order 5"), "FILLED"));
        CHECK(!has(rig.run("order 5"), "WARNING"));
        CHECK(healthy(*rig.manager));
    }
}

TEST(RecoveryNeverResubmitsOrReExecutesAnything) {
    for (long n : stalePoints()) {
        bool stale = false;
        Rig rig(staleManager(n, stale));
        const std::size_t trades = rig.manager->trades().size();
        const std::size_t records = rig.manager->recordedOrderCount();
        const std::size_t active = rig.manager->activeOrderCount();
        rig.run("order 5");
        rig.run("trades 5");
        rig.run("recover");
        rig.run("recover");
        CHECK_EQ(rig.manager->trades().size(), trades);
        CHECK_EQ(rig.manager->recordedOrderCount(), records);
        CHECK_EQ(rig.manager->activeOrderCount(), active);
    }
}

TEST(AMutatingCommandCompletesPendingBookkeepingFirst) {
    // Not the CLI's doing: OrderManager heals before every mutating call. The CLI shows it.
    bool stale = false;
    Rig rig(staleManager(stalePoints().front(), stale));
    CHECK(stale);
    const std::string o = rig.run("cancel 99");  // a harmless trading command
    CHECK(has(o, "order not found"));
    CHECK(rig.manager->isHistoryCurrent());
    CHECK(!has(rig.run("order 5"), "WARNING"));
    CHECK_EQ(recordedTradeIndices(*rig.manager), 6u);
    CHECK(healthy(*rig.manager));
}

// ---- A trading command that fails is reported, never retried ----------------------------------------------------

TEST(FailedSubmitIsReportedAndNotRetried) {
    Rig rig(std::make_unique<OrderManager>());
    const ParsedCommand cmd = parsed("submit BUY 100 5 9");
    {
        fault::FailAllocation starved(1, true);  // memory is gone from the first allocation
        rig.cli.execute(cmd);
    }
    const std::string err = rig.errors();
    CHECK(has(err, "out of memory"));
    CHECK(has(err, "Do NOT resubmit"));
    CHECK(has(err, "'recover'"));
    // The CLI did not try again: nothing was recorded (the manager rolled the failed submit back).
    CHECK_EQ(rig.manager->recordedOrderCount(), 0u);
    CHECK(rig.manager->trades().empty());
    CHECK(healthy(*rig.manager));
    // The session continues normally afterwards.
    CHECK(has(rig.run("submit BUY 100 5 9"), "ACCEPTED"));
}

TEST(SubmitThatFailsAfterTradingIsNeverResubmittedAndStaysConsistent) {
    bool found = false;
    for (long n = 1; n <= 60 && !found; ++n) {
        Rig rig(sellers(2));
        const ParsedCommand cmd = parsed("submit BUY 100 35 5");  // 2 trades, then 15 would rest
        {
            fault::FailAllocation starved(n, true);
            rig.errBuf.str("");
            rig.cli.execute(cmd);
        }
        const std::size_t executed = rig.manager->trades().size();
        if (executed == 0 || !has(rig.errBuf.str(), "Do NOT resubmit")) continue;  // failed too early
        found = true;

        CHECK(has(rig.errBuf.str(), "may already have executed"));
        // The user follows the advice: recover, then look. The trades stand, exactly once.
        CHECK(has(rig.run("recover"), "Recovery complete") || has(rig.run("recover"), "already current"));
        CHECK(rig.manager->isHistoryCurrent());
        CHECK_EQ(rig.manager->trades().size(), executed);
        const std::string order = rig.run("order 5");
        CHECK(has(order, "CANCELLED"));
        CHECK(has(order, "internal failure"));
        CHECK_EQ(rig.manager->tradesFor(5).size(), executed);
        CHECK_EQ(recordedTradeIndices(*rig.manager), 2 * executed);
        // Resubmitting the same id is refused: it is retired, so nothing can double-execute.
        const std::string again = rig.run("submit BUY 100 35 5");
        CHECK(has(again, "REJECTED"));
        CHECK(has(again, "duplicate order id"));
        CHECK_EQ(rig.manager->trades().size(), executed);
        CHECK(healthy(*rig.manager));
    }
    CHECK(found);  // the failure really did land after some trades
}

TEST(FailedCancelOrModifyUseTheSameWarning) {
    Rig rig(std::make_unique<OrderManager>());
    rig.run("submit BUY 100 10");
    const ParsedCommand modify = parsed("modify 1 101 10");  // price change: allocates (cancel/replace)
    {
        fault::FailAllocation starved(1, true);
        rig.errBuf.str("");
        rig.cli.execute(modify);
    }
    CHECK(has(rig.errBuf.str(), "Do NOT resubmit"));
    CHECK(rig.run("recover").size() > 0);
    CHECK(rig.manager->isHistoryCurrent());
    CHECK(healthy(*rig.manager));
}

TEST_MAIN()
