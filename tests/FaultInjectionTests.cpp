// Deterministic allocation-failure tests for the order book, matching engine and
// order manager. See FaultInjection.h for the design. This file (and only this
// file) replaces the global operator new; it is its own test executable.
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "FaultInjection.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "OrderManager.h"
#include "TestFramework.h"

// ---- The only global replacement, inert unless a FailAllocation guard is alive ----
// Kept out of line: if the compiler inlines `operator delete` into callers it can no
// longer see that the pointer came from this file's `operator new` (which it cannot
// prove is malloc-backed) and raises -Wmismatched-new-delete on a correct pairing.
#if defined(_MSC_VER)
#define LOB_TEST_NOINLINE __declspec(noinline)
#else
#define LOB_TEST_NOINLINE __attribute__((noinline))
#endif

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

// ---- Reporting helpers (never called while a guard is alive) ----------------------

bool ok(const OrderManager& m) {
    const InvariantReport r = m.checkConsistency();
    for (const auto& v : r.violations) std::cerr << "    consistency violated: " << v << "\n";
    return r.ok();
}

bool bookOk(const OrderBook& b) {
    const InvariantReport r = b.checkInvariants();
    for (const auto& v : r.violations) std::cerr << "    book invariant violated: " << v << "\n";
    return r.ok();
}

void note(const char* name, long points, const std::string& extra = "") {
    std::cout << "    [fault-injection] " << name << ": " << points << " allocation points failed"
              << extra << "\n";
}

// ---- The sweep: fail allocation #1, #2, ... until the operation no longer reaches it ----

template <typename Setup, typename Op, typename Verify>
long sweep(Setup setup, Op op, Verify verify, bool persistent = false) {
    long points = 0;
    for (long n = 1; n <= 500; ++n) {
        auto subject = setup();  // built disarmed, identical every time
        bool threw = false;
        long reached = 0;
        {
            fault::FailAllocation guard(n, persistent);
            try {
                op(*subject);
            } catch (const std::bad_alloc&) {
                threw = true;
            } catch (const std::exception& e) {
                std::cerr << "    unexpected exception: " << e.what() << "\n";
                CHECK(false);
            }
            reached = guard.allocations();
        }
        verify(*subject, threw, n);
        // The operation may legitimately SWALLOW an injected failure (deferred
        // history), so "did not throw" does not mean "n was past the last
        // allocation". Stop only when the Nth allocation was never even attempted.
        if (reached < n) return points;
        ++points;
    }
    CHECK(false);  // the operation never stopped allocating: sweep bound too small
    return points;
}

// Double failure: fail allocation #n and, as well, a LATER allocation #m (m > n). Because
// #m is numbered after #n, it typically lands inside the operation's own recovery
// from the first failure, e.g. after it has applied some trades and before the rest.
template <typename Setup, typename Op, typename Verify>
long sweepPairs(Setup setup, Op op, Verify verify) {
    long runs = 0;
    for (long n = 1; n <= 60; ++n) {
        bool anyReached = false;
        for (long m = n + 1; m <= n + 12; ++m) {
            auto subject = setup();
            bool threw = false;
            long reached = 0;
            {
                fault::FailAllocation guard(n, false, m);
                try {
                    op(*subject);
                } catch (const std::bad_alloc&) {
                    threw = true;
                } catch (const std::exception& e) {
                    std::cerr << "    unexpected exception: " << e.what() << "\n";
                    CHECK(false);
                }
                reached = guard.allocations();
            }
            if (reached < n) break;  // not even the first failure was reached
            anyReached = true;
            verify(*subject, threw, n);
            ++runs;
            if (reached < m) break;  // the second failure point is past the end: larger m add nothing
        }
        if (!anyReached) return runs;
    }
    CHECK(false);
    return runs;
}

using Mgr = std::unique_ptr<OrderManager>;

// Manager with `count` resting sells (ids 1..count) of `qty` at price 100.
Mgr sellers(int count, Quantity qty = 10) {
    Mgr m = std::make_unique<OrderManager>();
    for (int i = 1; i <= count; ++i) m->submit(static_cast<OrderId>(i), S, 100, qty);
    return m;
}

struct Stats {
    long pending = 0;      // outcomes where bookkeeping was pending after the call
    bool midway = false;   // a pending state with only some trades applied
};

// After a call that may have failed: audit the (possibly pending) state, then heal it.
// While history is not current the only violation allowed is the "not current" notice.
void settle(OrderManager& m, Stats& st) {
    CHECK(bookOk(m.book()));  // the book is valid no matter what
    if (!m.isHistoryCurrent()) {
        ++st.pending;
        const InvariantReport r = m.checkConsistency();
        CHECK_EQ(r.violations.size(), 1u);  // nothing misleading beyond the notice itself
        if (r.violations.size() == 1) CHECK_EQ(r.violations[0], std::string(kHistoryNotCurrent));
        // How many trades have been applied to records so far (each is on two records)?
        std::size_t applied = 0;
        for (OrderId id = 1; id <= 20; ++id) {
            if (auto rec = m.order(id)) applied += rec->tradeIndices.size();
        }
        applied /= 2;
        if (applied > 0 && applied < m.trades().size()) st.midway = true;
        // A starved recovery must neither throw nor corrupt anything.
        {
            fault::FailAllocation starved(1, true);
            m.recover();
        }
        CHECK(bookOk(m.book()));
        const InvariantReport again = m.checkConsistency();
        CHECK(again.violations.size() <= 1);
    }
    CHECK(m.recover());  // memory is back: the pending work completes
    CHECK(m.isHistoryCurrent());
    CHECK(ok(m));
}

// Total trade indices held by all records (each applied trade is on two records).
// Safe for ids that were never recorded.
std::size_t recordedTradeIndices(const OrderManager& m) {
    std::size_t total = 0;
    for (OrderId id = 1; id <= 20; ++id) {
        if (const auto rec = m.order(id)) total += rec->tradeIndices.size();
    }
    return total;
}

Quantity tradedFor(const OrderManager& m, OrderId id, std::size_t fromTrade = 0) {
    Quantity q = 0;
    const auto& ts = m.trades();
    for (std::size_t i = fromTrade; i < ts.size(); ++i) {
        if (ts[i].buyOrderId() == id || ts[i].sellOrderId() == id) q += ts[i].quantity();
    }
    return q;
}

}  // namespace

// ===== Injector self-tests (the harness is not destabilized) ===========================

// These call ::operator new / ::operator delete directly: a new-expression whose
// result is never really used may legally be elided by the optimizer, which would
// make a counting test meaningless.
TEST(InjectorIsInertWhenDisarmed) {
    for (int i = 0; i < 100; ++i) {
        void* p = ::operator new(16);
        CHECK(p != nullptr);
        ::operator delete(p);
    }
    CHECK(!fault::state().armed);
}

TEST(InjectorFailsExactlyTheNthAllocation) {
    void* ptrs[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    bool threw[5] = {false, false, false, false, false};
    {
        fault::FailAllocation g(3);
        for (int i = 0; i < 5; ++i) {
            try {
                ptrs[i] = ::operator new(16);
            } catch (const std::bad_alloc&) {
                threw[i] = true;
            }
        }
    }
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(threw[i], i == 2);
        if (ptrs[i] != nullptr) ::operator delete(ptrs[i]);
    }
}

TEST(InjectorPersistentModeFailsEveryLaterAllocation) {
    void* ptrs[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    bool threw[5] = {false, false, false, false, false};
    {
        fault::FailAllocation g(2, true);
        for (int i = 0; i < 5; ++i) {
            try {
                ptrs[i] = ::operator new(16);
            } catch (const std::bad_alloc&) {
                threw[i] = true;
            }
        }
    }
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(threw[i], i >= 1);
        if (ptrs[i] != nullptr) ::operator delete(ptrs[i]);
    }
}

TEST(InjectorCountsWithoutFailingAndResetsOnScopeExit) {
    long seen = 0;
    void* ptrs[4] = {nullptr, nullptr, nullptr, nullptr};
    {
        fault::FailAllocation g(0);  // count only
        for (int i = 0; i < 4; ++i) ptrs[i] = ::operator new(16);
        seen = g.allocations();
    }
    CHECK_EQ(seen, 4);
    for (void* p : ptrs) ::operator delete(p);
    CHECK(!fault::state().armed);
    std::vector<int> v(1000, 1);  // allocation works again after the guard
    CHECK_EQ(v.size(), 1000u);
}

// ===== OrderBook =========================================================================

namespace {
Sequence g_seq = 0;
Order mkOrder(OrderId id, Side side, Price price, Quantity qty) {
    return *Order::create(id, side, OrderType::Limit, price, qty, ++g_seq);
}
}  // namespace

TEST(FaultOrderBookAddIsStrongAtEveryAllocation) {
    long points = 0;
    for (int size = 0; size <= 30; ++size) {      // crosses the hash table's rehash thresholds
        for (int sameLevel = 0; sameLevel <= 1; ++sameLevel) {
            const std::size_t levels = size < 7 ? static_cast<std::size_t>(size) : 7u;
            points += sweep(
                [&] {
                    auto b = std::make_unique<OrderBook>();
                    for (int i = 0; i < size; ++i) {
                        b->add(mkOrder(static_cast<OrderId>(i + 1), B, 100 + (i % 7), 5));
                    }
                    return b;
                },
                [&](OrderBook& b) { b.add(mkOrder(1000, B, sameLevel ? 100 : 500, 7)); },
                [&](OrderBook& b, bool threw, long) {
                    CHECK(bookOk(b));
                    if (threw) {  // strong: exactly as before
                        CHECK(!b.contains(1000));
                        CHECK_EQ(b.orderCount(), static_cast<std::size_t>(size));
                        CHECK_EQ(b.bidLevelCount(), levels);
                    } else {
                        CHECK(b.contains(1000));
                        CHECK_EQ(b.orderCount(), static_cast<std::size_t>(size) + 1);
                    }
                });
        }
    }
    CHECK(points > 100);
    note("OrderBook::add (sizes 0..30, new and existing level)", points);
}

TEST(FaultOrderBookQueriesAreStatelessUnderFailure) {
    long points = sweep(
        [] {
            auto b = std::make_unique<OrderBook>();
            for (int i = 0; i < 5; ++i) b->add(mkOrder(static_cast<OrderId>(i + 1), B, 100 + i, 5));
            return b;
        },
        [](OrderBook& b) { (void)b.depth(10); },
        [](OrderBook& b, bool, long) {
            CHECK(bookOk(b));
            CHECK_EQ(b.orderCount(), 5u);
        });
    CHECK(points > 0);
    note("OrderBook::depth", points);
}

TEST(FaultOrderBookRemovalsAndFillsNeverAllocate) {
    OrderBook book;
    for (int i = 1; i <= 6; ++i) book.add(mkOrder(static_cast<OrderId>(i), S, 100 + (i % 2), 10));
    long extract = 0, cancel = 0, reduce = 0, fill = 0, full = 0;
    { fault::FailAllocation g(0); book.extract(1); extract = g.allocations(); }
    { fault::FailAllocation g(0); book.cancel(2); cancel = g.allocations(); }
    { fault::FailAllocation g(0); book.reduce(3, 4); reduce = g.allocations(); }
    { fault::FailAllocation g(0); book.fill(4, 3); fill = g.allocations(); }
    { fault::FailAllocation g(0); book.fill(4, 7); full = g.allocations(); }  // removes it + maybe a level
    CHECK_EQ(extract, 0);
    CHECK_EQ(cancel, 0);
    CHECK_EQ(reduce, 0);
    CHECK_EQ(fill, 0);
    CHECK_EQ(full, 0);
    CHECK(bookOk(book));
}

// ===== MatchingEngine: basic guarantee ====================================================

TEST(FaultEngineSubmitKeepsBookValidAndTradesExact) {
    Stats lostRemainder;
    long points = sweep(
        [] {
            auto e = std::make_unique<MatchingEngine>();
            for (int i = 1; i <= 3; ++i) {
                e->submit(mkOrder(static_cast<OrderId>(i), S, 100, 10));
            }
            return e;
        },
        [](MatchingEngine& e) { e.submit(mkOrder(9, B, 100, 35)); },  // 3 trades, rest 5
        [&](MatchingEngine& e, bool threw, long) {
            CHECK(bookOk(e.book()));
            Quantity traded = 0;
            TradeId last = 0;
            for (const Trade& t : e.trades()) {
                CHECK(t.id() > last);  // unique, increasing (gaps allowed after a failure)
                last = t.id();
                traded += t.quantity();
            }
            // Conservation: everything that left the asks is in exactly one trade.
            Quantity asks = 0;
            for (const auto& lvl : e.book().asks()) asks += lvl.second.totalQuantity();
            CHECK_EQ(asks, 30 - traded);
            if (!threw) {
                CHECK_EQ(traded, 30);
                CHECK(e.book().contains(9));
            } else if (!e.book().contains(9)) {
                lostRemainder.pending++;  // order not rested after a failure: documented basic guarantee
            }
        });
    CHECK(points > 0);
    CHECK(lostRemainder.pending > 0);  // the basic-guarantee window really is exercised
    note("MatchingEngine::submit", points);
}

// ===== OrderManager =======================================================================

TEST(FaultSubmitThatRestsIsStrong) {
    for (int persistent = 0; persistent <= 1; ++persistent) {
        Stats st;
        long points = sweep(
            [] {
                Mgr m = std::make_unique<OrderManager>();
                m->submit(1, B, 99, 10);
                return m;
            },
            [](OrderManager& m) { m.submit(5, B, 100, 30); },
            [&](OrderManager& m, bool threw, long) {
                settle(m, st);
                if (!threw) {
                    CHECK(m.book().contains(5));
                    CHECK(m.order(5)->status == OrderStatus::New);
                } else {  // nothing executed: rolled back completely, the id stays free
                    CHECK(!m.knows(5));
                    CHECK(!m.book().contains(5));
                    CHECK_EQ(m.book().orderCount(), 1u);
                    CHECK_EQ(m.recordedOrderCount(), 1u);
                    CHECK(m.trades().empty());
                    CHECK(m.submit(5, B, 100, 30).accepted);
                    CHECK(ok(m));
                }
            },
            persistent == 1);
        CHECK(points > 0);
        note(persistent ? "submit, resting (persistent)" : "submit, resting", points);
    }
}

TEST(FaultSubmitWithGeneratedIdDoesNotLoseOrReuseIds) {
    Stats st;
    long points = sweep(
        [] { return std::make_unique<OrderManager>(); },
        [](OrderManager& m) { m.submit(B, 100, 5); },
        [&](OrderManager& m, bool threw, long) {
            settle(m, st);
            if (threw) {
                CHECK_EQ(m.recordedOrderCount(), 0u);
                ExecutionReport next = m.submit(B, 100, 5);  // generation carries on
                CHECK(next.accepted);
                CHECK(ok(m));
            } else {
                CHECK_EQ(m.recordedOrderCount(), 1u);
            }
        });
    CHECK(points > 0);
    note("submit, generated id", points);
}

TEST(FaultRejectedSubmissionRecordCreation) {
    Stats st;
    long points = sweep(
        [] { return std::make_unique<OrderManager>(); },
        [](OrderManager& m) { m.submit(7, B, 0, 10); },  // invalid price: recorded as REJECTED
        [&](OrderManager& m, bool threw, long) {
            settle(m, st);
            if (threw) {
                CHECK(!m.knows(7));  // no half-made record
                CHECK(m.submit(7, B, 0, 10).rejectReason == RejectReason::InvalidPrice);
                CHECK(m.knows(7));
            } else {
                CHECK(m.order(7)->status == OrderStatus::Rejected);
            }
            CHECK(ok(m));
        });
    CHECK(points > 0);
    note("submit, rejected record", points);
}

TEST(FaultCrossingSubmitFullyFilled) {
    for (int persistent = 0; persistent <= 1; ++persistent) {
        Stats st;
        long points = sweep(
            [] { return sellers(3); },
            [](OrderManager& m) { m.submit(5, B, 100, 25); },  // trades 10, 10, 5
            [&](OrderManager& m, bool threw, long) {
                settle(m, st);
                const std::size_t t = m.trades().size();
                if (!threw) {
                    CHECK_EQ(t, 3u);
                    CHECK(m.order(5)->status == OrderStatus::Filled);
                    CHECK_EQ(m.order(5)->executedQuantity, 25);
                } else if (t == 0) {  // failed before the first execution: strong
                    CHECK(!m.knows(5));
                    CHECK_EQ(m.book().orderCount(), 3u);
                    for (OrderId id = 1; id <= 3; ++id) CHECK_EQ(m.order(id)->remainingQuantity, 10);
                } else {  // executions stand; the unfinished order is reported lost
                    auto r = m.order(5);
                    CHECK(r.has_value());
                    CHECK(r->status == OrderStatus::Cancelled);
                    CHECK(r->cancelReason == CancelReason::SystemFailure);
                    CHECK_EQ(r->executedQuantity, tradedFor(m, 5));
                    CHECK_EQ(r->cancelledQuantity, 25 - r->executedQuantity);
                    CHECK_EQ(r->remainingQuantity, 0);
                    CHECK_EQ(r->tradeIndices.size(), t);
                    CHECK(!m.book().contains(5));
                    CHECK(m.submit(5, B, 100, 1).rejectReason == RejectReason::DuplicateOrderId);
                }
                CHECK(ok(m));
                // Each maker's record agrees with what actually traded.
                for (OrderId id = 1; id <= 3; ++id) {
                    CHECK_EQ(m.order(id)->executedQuantity, tradedFor(m, id));
                }
            },
            persistent == 1);
        CHECK(points > 0);
        note(persistent ? "crossing submit, fully filled (persistent)" : "crossing submit, fully filled",
             points, ", pending-history outcomes: " + std::to_string(st.pending) +
                         (st.midway ? " (incl. a partly applied one)" : ""));
    }
}

TEST(FaultCrossingSubmitThenRestRemainder) {
    // mode 0: one failure; 1: memory stays exhausted; 2: a second failure during recovery
    for (int mode = 0; mode <= 2; ++mode) {
        Stats st;
        bool sawLost = false;
        auto setup = [] { return sellers(2); };
        auto op = [](OrderManager& m) { m.submit(5, B, 100, 35); };  // trades 10, 10, then rests 15
        auto verify = [&](OrderManager& m, bool threw, long) {
            settle(m, st);
            const std::size_t t = m.trades().size();
            if (!threw && t == 2) {
                auto r = m.order(5);
                CHECK(r->status == OrderStatus::PartiallyFilled);
                CHECK_EQ(r->executedQuantity, 20);
                CHECK_EQ(r->remainingQuantity, 15);
                CHECK(m.book().contains(5));
            } else if (t == 0) {
                CHECK(!m.knows(5));
                CHECK_EQ(m.book().orderCount(), 2u);
            } else {
                sawLost = true;
                auto r = m.order(5);
                CHECK(r->status == OrderStatus::Cancelled);
                CHECK(r->cancelReason == CancelReason::SystemFailure);
                CHECK_EQ(r->executedQuantity, tradedFor(m, 5));
                CHECK_EQ(r->cancelledQuantity, 35 - r->executedQuantity);
                CHECK(!m.book().contains(5));
                CHECK_EQ(m.book().bidLevelCount(), 0u);  // no empty level left behind
                CHECK(m.submit(5, B, 100, 1).rejectReason == RejectReason::DuplicateOrderId);
            }
            CHECK(ok(m));
        };
        long points = mode == 2 ? sweepPairs(setup, op, verify) : sweep(setup, op, verify, mode == 1);
        CHECK(points > 0);
        CHECK(sawLost);  // failing the rest after trades really loses the order (documented)
        const char* names[] = {"crossing submit then rest", "crossing submit then rest (persistent)",
                               "crossing submit then rest (double failure)"};
        note(names[mode], points);
    }
}

TEST(FaultDeferredHistoryIsCompletedByTheNextCall) {
    bool sawDeferred = false;
    Stats st;
    long points = sweep(
        [] { return sellers(2); },
        [](OrderManager& m) { m.submit(5, B, 100, 35); },
        [&](OrderManager& m, bool threw, long) {
            if (!threw && !m.isHistoryCurrent()) {
                // The submit SUCCEEDED (it returned normally); only the bookkeeping lagged.
                sawDeferred = true;
                CHECK(bookOk(m.book()));
                CHECK(m.book().contains(5));
                ExecutionReport c = m.cancel(5);  // heals first, then cancels correctly
                CHECK(c.accepted);
                CHECK_EQ(c.executedQuantity, 20);
                CHECK_EQ(c.remainingQuantity, 0);
                auto r = m.order(5);
                CHECK_EQ(r->executedQuantity, 20);
                CHECK_EQ(r->cancelledQuantity, 15);
                CHECK(m.isHistoryCurrent());
                CHECK(ok(m));
            } else {
                settle(m, st);
            }
        });
    CHECK(points > 0);
    CHECK(sawDeferred);
    note("deferred history completed by next call", points);
}

TEST(FaultInterruptedTradeApplicationResumesWithoutDuplicatesOrGaps) {
    bool sawMidway = false;
    // Fail inside the history update after the engine has finished 3 trades: the cursor
    // may stop part-way. Whatever happens, recovery applies each trade exactly once.
    Stats st;
    long points = sweep(
        [] { return sellers(3); },
        [](OrderManager& m) { m.submit(5, B, 100, 30); },
        [&](OrderManager& m, bool threw, long) {
            if (!threw) {
                if (!m.isHistoryCurrent()) {
                    // How far did the cursor get? (each applied trade is on two records)
                    std::size_t applied = 0;
                    applied = recordedTradeIndices(m);
                    applied /= 2;
                    if (applied > 0 && applied < m.trades().size()) sawMidway = true;
                    // Snapshots lag but never contradict themselves.
                    const InvariantReport r = m.checkConsistency();
                    CHECK_EQ(r.violations.size(), 1u);
                    // Resume twice: the second call must be a harmless no-op.
                    CHECK(m.recover());
                    CHECK(m.recover());
                }
                CHECK(m.isHistoryCurrent());
                CHECK(ok(m));
                std::size_t indices = 0;
                indices = recordedTradeIndices(m);
                CHECK_EQ(indices, 2 * m.trades().size());  // each trade on exactly two records
                CHECK_EQ(m.trades().size(), 3u);           // and none lost
                for (OrderId id = 1; id <= 3; ++id) {
                    CHECK_EQ(m.order(id)->tradeIndices.size(), 1u);
                    CHECK(m.order(id)->status == OrderStatus::Filled);
                }
                CHECK_EQ(m.order(5)->tradeIndices.size(), 3u);
            } else {
                settle(m, st);
            }
        });
    CHECK(points > 0);
    CHECK(sawMidway);  // the cursor really did stop part-way, and resumed correctly
    note("interrupted trade application", points);
}

TEST(FaultCancelAndInPlaceReduceNeverAllocate) {
    Mgr m = sellers(3, 10);
    long cancel = -1, unknown = -1, reduce = -1, noChange = -1, filledCancel = -1;
    { fault::FailAllocation g(0); m->cancel(1); cancel = g.allocations(); }
    { fault::FailAllocation g(0); m->cancel(99); unknown = g.allocations(); }
    { fault::FailAllocation g(0); m->modify(2, std::nullopt, 4); reduce = g.allocations(); }
    { fault::FailAllocation g(0); m->modify(2, std::nullopt, 4); noChange = g.allocations(); }
    m->submit(9, B, 100, 4);  // fills order 2's remainder...
    { fault::FailAllocation g(0); m->cancel(2); filledCancel = g.allocations(); }
    CHECK_EQ(cancel, 0);        // so they cannot fail with bad_alloc: strong, trivially
    CHECK_EQ(unknown, 0);
    CHECK_EQ(reduce, 0);
    CHECK_EQ(noChange, 0);
    CHECK_EQ(filledCancel, 0);
    CHECK(ok(*m));
}

TEST(FaultModifyReplaceNonCrossing) {
    for (int persistent = 0; persistent <= 1; ++persistent) {
        Stats st;
        bool sawLost = false;
        long points = sweep(
            [] {
                Mgr m = std::make_unique<OrderManager>();
                m->submit(5, B, 100, 10);
                m->submit(6, B, 100, 10);
                return m;
            },
            [](OrderManager& m) { m.modify(5, 101, std::nullopt); },
            [&](OrderManager& m, bool threw, long) {
                settle(m, st);
                auto r = m.order(5);
                CHECK(r.has_value());
                if (!threw) {
                    CHECK_EQ(m.book().find(5)->price(), 101);
                    CHECK(r->sequence > r->submissionSequence);
                } else if (m.book().contains(5)) {  // failed before the order left the book
                    CHECK_EQ(m.book().find(5)->price(), 100);
                    CHECK(r->status == OrderStatus::New);
                    CHECK_EQ(r->sequence, r->submissionSequence);  // priority untouched
                    CHECK_EQ(m.book().orderCount(), 2u);
                } else {  // taken out, could not be re-entered: lost, not duplicated
                    sawLost = true;
                    CHECK(r->status == OrderStatus::Cancelled);
                    CHECK(r->cancelReason == CancelReason::SystemFailure);
                    CHECK_EQ(r->cancelledQuantity, 10);
                    CHECK_EQ(r->executedQuantity, 0);
                    CHECK_EQ(r->remainingQuantity, 0);
                    CHECK_EQ(m.book().orderCount(), 1u);
                }
                CHECK(m.submit(5, B, 100, 1).rejectReason == RejectReason::DuplicateOrderId);
            },
            persistent == 1);
        CHECK(points > 0);
        CHECK(sawLost);
        note(persistent ? "modify price, no cross (persistent)" : "modify price, no cross", points);
    }
}

TEST(FaultModifyReplaceCrossingKeepsExecutedQuantity) {
    for (int mode = 0; mode <= 2; ++mode) {
        Stats st;
        std::size_t tradesBefore = 0;
        auto setup = [&] {
            Mgr m = std::make_unique<OrderManager>();
            m->submit(5, B, 100, 20);
            m->submit(9, S, 100, 5);   // executes 5 of order 5 (remaining 15)
            m->submit(1, S, 105, 5);
            m->submit(2, S, 105, 5);
            m->submit(3, S, 105, 5);
            tradesBefore = m->trades().size();
            return m;
        };
        auto op = [](OrderManager& m) { m.modify(5, 105, std::nullopt); };  // crosses: 3 trades of 5
        auto verify = [&](OrderManager& m, bool threw, long) {
            settle(m, st);
            auto r = m.order(5);
            CHECK(r.has_value());
            const std::size_t t = m.trades().size() - tradesBefore;
            CHECK(r->executedQuantity >= 5);  // never rolled back
            CHECK_EQ(r->executedQuantity, tradedFor(m, 5));
            if (!threw && t == 3) {
                CHECK(r->status == OrderStatus::Filled);
                CHECK_EQ(r->executedQuantity, 20);
            } else if (t == 0) {
                CHECK_EQ(r->executedQuantity, 5);
                if (m.book().contains(5)) {
                    CHECK_EQ(m.book().find(5)->price(), 100);
                    CHECK_EQ(r->remainingQuantity, 15);
                } else {
                    CHECK(r->status == OrderStatus::Cancelled);
                    CHECK_EQ(r->cancelledQuantity, 15);
                }
            } else {  // it traded under its new terms, then could not be finished
                CHECK(r->status == OrderStatus::Cancelled);
                CHECK(r->cancelReason == CancelReason::SystemFailure);
                CHECK_EQ(r->price, 105);
                CHECK_EQ(r->effectiveQuantity, 20);
                CHECK_EQ(r->cancelledQuantity, 20 - r->executedQuantity);
                CHECK_EQ(r->tradeIndices.size(), 1 + t);
                CHECK(!m.book().contains(5));
            }
            CHECK(m.submit(5, B, 100, 1).rejectReason == RejectReason::DuplicateOrderId);
            CHECK(ok(m));
        };
        long points = mode == 2 ? sweepPairs(setup, op, verify) : sweep(setup, op, verify, mode == 1);
        CHECK(points > 0);
        const char* names[] = {"modify price, crossing", "modify price, crossing (persistent)",
                               "modify price, crossing (double failure)"};
        note(names[mode], points);
    }
}

TEST(FaultManagerQueriesAreStatelessUnderFailure) {
    Stats st;
    long points = sweep(
        [] {
            Mgr m = sellers(1);
            m->submit(5, B, 100, 10);  // one trade on orders 1 and 5
            return m;
        },
        [](OrderManager& m) {
            (void)m.order(1);
            (void)m.tradesFor(1);
        },
        [&](OrderManager& m, bool, long) {
            settle(m, st);
            CHECK_EQ(m.order(1)->tradeIndices.size(), 1u);
            CHECK_EQ(m.tradesFor(5).size(), 1u);
        });
    CHECK(points > 0);
    note("order() / tradesFor()", points);
}

TEST(RecoverOnAHealthyManagerIsANoOp) {
    Mgr m = sellers(2);
    m->submit(5, B, 100, 15);
    CHECK(m->isHistoryCurrent());
    CHECK(m->recover());
    CHECK(m->recover());
    CHECK(ok(*m));
    CHECK_EQ(m->trades().size(), 2u);
}

TEST_MAIN()
