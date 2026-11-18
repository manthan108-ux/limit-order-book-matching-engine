// System-level fault injection (Phase 7). Its own test executable: it replaces the global
// operator new exactly like FaultInjectionTests.cpp / CliRecoveryTests.cpp (see
// FaultInjection.h for the isolation argument), and additionally COUNTS live
// allocations, so that leaks and double frees on the failure paths are detected.
//
// What is checked here is not a hand-written expectation per failure point (the
// Phase 5.1 suite does that) but the SYSTEM invariants (SystemAuditor.h) after every
// injected failure: book integrity, lifecycle <-> book agreement, quantity
// reconciliation, trade associations, id retirement, transitions, and stale-history
// accuracy. The reference model is not used here: after an injected failure the
// correct outcome is deliberately not one single answer (rolled back, or lost with
// SystemFailure), so the model could not predict it.
//
// Guarantee vocabulary used in the assertions (see docs/ARCHITECTURE.md):
//   strong   = an exception leaves the state exactly as it was
//   basic    = valid, consistent state, but the operation is only partly done
//   deferred = the operation succeeded; the history bookkeeping catches up later
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "FaultInjection.h"
#include "OrderManager.h"
#include "SystemAuditor.h"
#include "TestFramework.h"
#include "TestRng.h"

#if defined(_MSC_VER)
#define LOB_TEST_NOINLINE __declspec(noinline)
#else
#define LOB_TEST_NOINLINE __attribute__((noinline))
#endif

namespace acct {
long live = 0;   // currently outstanding allocations made through operator new
long total = 0;  // allocations made in total
}  // namespace acct

LOB_TEST_NOINLINE void* operator new(std::size_t size) {
    if (fault::shouldFail()) throw std::bad_alloc();
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) throw std::bad_alloc();
    ++acct::live;
    ++acct::total;
    return p;
}
LOB_TEST_NOINLINE void operator delete(void* p) noexcept {
    if (p == nullptr) return;
    --acct::live;
    std::free(p);
}
LOB_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept {
    if (p == nullptr) return;
    --acct::live;
    std::free(p);
}

using namespace lob;

namespace {

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size() && i < 6; ++i) out += "\n      " + v[i];
    return out;
}

// The kinds of failure plan.
enum class Mode { None, Single, Persistent, Double };

struct Plan {
    Mode mode = Mode::None;
    long first = 1;
    long second = 0;
};

}  // namespace

// ===== Allocation accounting: no leaks, no double frees ============================================

TEST(AllocationAccountingBalancesForANormalWorkload) {
    const long before = acct::live;
    {
        OrderManager m;
        for (OrderId i = 1; i <= 300; ++i) {
            m.submit(i, i % 2 ? B : S, 100 + static_cast<Price>(i % 5), 1 + static_cast<Quantity>(i % 7));
            if (i % 3 == 0) m.cancel(i - 1);
            if (i % 5 == 0) m.modify(i - 2, std::nullopt, 9);
        }
        CHECK(m.book().checkInvariants().ok());
    }  // manager destroyed here
    if (acct::live != before) std::cerr << "    live allocation delta: " << (acct::live - before) << std::endl;
    CHECK_EQ(acct::live, before);  // everything allocated was freed exactly once
}

TEST(AllocationAccountingProvesTheCounterWorks) {
    const long before = acct::live;
    void* leak = ::operator new(64);
    CHECK_EQ(acct::live, before + 1);   // an outstanding allocation is visible...
    ::operator delete(leak);
    CHECK_EQ(acct::live, before);       // ...and returns to the baseline when freed
}

// ===== Randomized workloads with injected failures =====================================================

namespace {

struct FaultStats {
    long ops = 0;
    long armed = 0;
    long threw = 0;
    long deferredHistory = 0;     // the op returned normally but history was left pending
    long systemFailureOrders = 0; // orders lost to a failure (basic guarantee) and reported so
    long rolledBackSubmits = 0;   // submits that threw and left no record at all (strong)
    long starvedRecoveries = 0;
};

class FaultWorkload {
public:
    FaultWorkload(std::uint64_t seed, int ops) : seed_(seed), ops_(ops), rng_(seed) {}

    FaultStats run() {
        for (int i = 0; i < ops_ && !failed_; ++i) step(i);
        // Final: memory is back, everything must settle to a fully current, fully clean state.
        if (!failed_) {
            CHECK_MSG(manager_.recover(), context(-1, "final recover") + ": recovery did not complete");
            auditCurrent(-1, "final");
        }
        return stats_;
    }

    OrderManager& manager() { return manager_; }

private:
    std::string context(int op, const std::string& what) const {
        return "[system-fault seed=" + std::to_string(seed_) + " op#" + std::to_string(op) + " " + what + "]";
    }

    void fail(int op, const std::string& what, const std::vector<std::string>& details = {}) {
        failed_ = true;
        std::cerr << "    FAULT-WORKLOAD FAILURE " << context(op, what) << join(details) << "\n";
        CHECK(false);
    }

    Plan pickPlan() {
        Plan p;
        if (rng_.chance(50)) {
            const std::uint64_t k = rng_.below(3);
            p.mode = k == 0 ? Mode::Single : k == 1 ? Mode::Persistent : Mode::Double;
            p.first = 1 + static_cast<long>(rng_.below(10));
            p.second = p.mode == Mode::Double ? p.first + 1 + static_cast<long>(rng_.below(8)) : 0;
        }
        return p;
    }

    // Runs op() under the plan. Returns true if it threw bad_alloc.
    template <typename Op>
    bool armedRun(const Plan& plan, Op&& op) {
        bool threw = false;
        if (plan.mode == Mode::None) {
            op();
            return false;
        }
        fault::FailAllocation guard(plan.first, plan.mode == Mode::Persistent, plan.second);
        try {
            op();
        } catch (const std::bad_alloc&) {
            threw = true;
        } catch (const std::exception& e) {
            std::cerr << "    unexpected exception type: " << e.what() << "\n";
            throw;
        }
        return threw;
    }

    OrderId pickTarget() {
        // Prefer an active order, else any tracked id, sometimes an unknown one.
        if (ids_.empty() || rng_.chance(4)) return 7000000 + rng_.below(100);
        for (int a = 0; a < 6 && rng_.chance(85); ++a) {
            const OrderId id = ids_[rng_.below(ids_.size())];
            const auto rec = manager_.order(id);
            if (rec && rec->isActive()) return id;
        }
        return ids_[rng_.below(ids_.size())];
    }

    void auditCurrent(int op, const std::string& when) {
        if (!manager_.isHistoryCurrent()) fail(op, when + ": history should be current here");
        const auto v = auditor_.audit(manager_, ids_);
        if (!v.empty()) fail(op, when + ": system invariant violation(s)", v);
    }

    void step(int op) {
        ++stats_.ops;
        const Plan plan = pickPlan();
        if (plan.mode != Mode::None) ++stats_.armed;

        // Statuses before, to detect any order that silently disappears or is cancelled by accident.
        std::map<OrderId, std::pair<OrderStatus, CancelReason>> before;
        for (OrderId id : ids_) {
            if (const auto rec = manager_.order(id)) before[id] = {rec->status, rec->cancelReason};
        }
        const std::size_t tradesBefore = manager_.trades().size();

        const std::uint64_t kind = rng_.below(100);
        OrderId target = 0;
        bool threw = false;
        bool isSubmit = false;
        if (kind < 45) {  // submit with a client id (so the id is known even if it throws)
            isSubmit = true;
            target = 1000000 + nextClient_++;
            const Side side = rng_.chance(50) ? B : S;
            const Price price = 100 + static_cast<Price>(rng_.below(7)) - 3;
            const Quantity qty = 1 + static_cast<Quantity>(rng_.below(30));
            threw = armedRun(plan, [&] { manager_.submit(target, side, price, qty); });
        } else if (kind < 55) {  // submit with a generated id: only when not injecting (the id is unknowable after a throw)
            isSubmit = true;
            const Side side = rng_.chance(50) ? B : S;
            const ExecutionReport r = manager_.submit(side, 100 + static_cast<Price>(rng_.below(7)) - 3,
                                                      1 + static_cast<Quantity>(rng_.below(30)));
            target = r.orderId;
        } else if (kind < 75) {  // cancel
            target = pickTarget();
            threw = armedRun(plan, [&] { manager_.cancel(target); });
        } else {  // modify
            target = pickTarget();
            std::optional<Price> p;
            std::optional<Quantity> q;
            if (rng_.chance(60)) p = 100 + static_cast<Price>(rng_.below(7)) - 3;
            if (rng_.chance(60)) q = 1 + static_cast<Quantity>(rng_.below(30));
            threw = armedRun(plan, [&] { manager_.modify(target, p, q); });
        }
        if (threw) ++stats_.threw;

        // Track a newly recorded id.
        if (target != 0 && manager_.knows(target) && !tracked(target)) ids_.push_back(target);
        if (isSubmit && threw && !manager_.knows(target)) ++stats_.rolledBackSubmits;

        // ---- Audit immediately: possibly mid-recovery (stale), which the auditor understands ----
        const bool stale = !manager_.isHistoryCurrent();
        if (!threw && stale) ++stats_.deferredHistory;
        const auto v = auditor_.audit(manager_, ids_);
        if (!v.empty()) {
            fail(op, std::string("after ") + (threw ? "a FAILED " : "an ") + "operation" + (stale ? " (history pending)" : ""), v);
            return;
        }

        // ---- No order may disappear or be cancelled by accident ----
        // The only legitimate surprise is an order lost to a FAILED operation of its own: either
        // this operation, or an earlier failed one whose recovery was still pending and which this
        // call has just completed (recovery is finished at the start of the next mutating call).
        if (threw) failedTargets_.insert(target);
        for (const auto& kv : before) {
            const auto now = manager_.order(kv.first);
            if (!now) { fail(op, "order " + std::to_string(kv.first) + " vanished from the manager"); return; }
            const bool wasActive = kv.second.first == OrderStatus::New || kv.second.first == OrderStatus::PartiallyFilled;
            const bool lostToFailure = now->cancelReason == CancelReason::SystemFailure && failedTargets_.count(kv.first) != 0;
            if (wasActive && now->status == OrderStatus::Cancelled && kv.first != target && !lostToFailure) {
                fail(op, "order " + std::to_string(kv.first) + " was cancelled although it was not the operation's target");
                return;
            }
            if (now->cancelReason == CancelReason::SystemFailure && kv.second.second != CancelReason::SystemFailure) {
                ++stats_.systemFailureOrders;
                if (failedTargets_.count(kv.first) == 0) { fail(op, "a SystemFailure cancellation on an order whose operation never failed"); return; }
            }
        }
        // A new record that is SystemFailure-cancelled can only come from a failed submit that had already traded.
        if (const auto rec = manager_.order(target); rec && rec->cancelReason == CancelReason::SystemFailure && before.count(target) == 0) {
            ++stats_.systemFailureOrders;
            if (!threw || !isSubmit || rec->tradeIndices.empty()) { fail(op, "a SystemFailure cancellation without a failed, trading submit"); return; }
        }
        if (manager_.trades().size() < tradesBefore) { fail(op, "the trade list shrank"); return; }

        // Sometimes a recovery is attempted while memory is still exhausted (must not throw or corrupt).
        if (stale && rng_.chance(40)) {
            ++stats_.starvedRecoveries;
            {
                fault::FailAllocation starved(1, true);
                manager_.recover();   // noexcept by contract
            }
            const auto w = auditor_.audit(manager_, ids_);
            if (!w.empty()) { fail(op, "after a starved recovery attempt", w); return; }
        }
        // Often heal immediately; otherwise leave it pending for the next mutating call to complete.
        if (rng_.chance(55)) {
            if (!manager_.recover()) { fail(op, "recover() with memory available did not complete"); return; }
            auditCurrent(op, "after recover()");
        }
    }

    bool tracked(OrderId id) const {
        for (OrderId x : ids_) {
            if (x == id) return true;
        }
        return false;
    }

    std::uint64_t seed_;
    int ops_;
    TestRng rng_;
    OrderManager manager_;
    audit::Auditor auditor_;
    std::vector<OrderId> ids_;
    OrderId nextClient_ = 1;
    FaultStats stats_;
    std::set<OrderId> failedTargets_;   // ids whose operation threw (their recovery may complete later)
    bool failed_ = false;
};

}  // namespace

TEST(RandomizedWorkloadsWithInjectedFailuresKeepEverySystemInvariant) {
    FaultStats total;
    for (const std::uint64_t seed : {101u, 202u, 303u, 404u, 505u, 606u}) {
        FaultWorkload w(seed, 350);
        const FaultStats s = w.run();
        total.ops += s.ops;
        total.armed += s.armed;
        total.threw += s.threw;
        total.deferredHistory += s.deferredHistory;
        total.systemFailureOrders += s.systemFailureOrders;
        total.rolledBackSubmits += s.rolledBackSubmits;
        total.starvedRecoveries += s.starvedRecoveries;
    }
    std::cout << "    [system-fault] " << total.ops << " ops, " << total.armed << " with injected failure, " << total.threw
              << " threw, " << total.deferredHistory << " deferred-history, " << total.rolledBackSubmits
              << " strong rollbacks, " << total.systemFailureOrders << " SystemFailure orders, " << total.starvedRecoveries
              << " starved recoveries\n";
    // The workload must really have exercised each outcome class, or it proves little.
    CHECK(total.armed > 500);
    CHECK(total.threw > 60);
    CHECK(total.deferredHistory > 5);
    CHECK(total.rolledBackSubmits > 5);
    CHECK(total.systemFailureOrders > 0);
    CHECK(total.starvedRecoveries > 5);
}

TEST(FailureWorkloadsDoNotLeakOrDoubleFree) {
    const long before = acct::live;
    {
        FaultWorkload a(777, 300);
        a.run();
        FaultWorkload b(888, 300);
        b.run();
    }
    CHECK_EQ(acct::live, before);   // every allocation on every failure / recovery path was released once
}

// ===== Recovery sweeps: single, persistent and double failures, with repeated and starved recovery ============

namespace {

struct Scenario {
    std::unique_ptr<OrderManager> manager;
    std::vector<OrderId> ids;
};

Scenario crossingSubmitScenario() {
    Scenario s{std::make_unique<OrderManager>(), {}};
    for (OrderId i = 1; i <= 4; ++i) s.manager->submit(i, S, 100, 5);
    s.manager->submit(5, B, 90, 9);
    s.ids = {1, 2, 3, 4, 5};
    return s;
}

Scenario replaceCrossingScenario() {
    Scenario s{std::make_unique<OrderManager>(), {}};
    s.manager->submit(5, B, 100, 20);
    s.manager->submit(9, S, 100, 5);   // order 5 has executed 5
    for (OrderId i = 1; i <= 3; ++i) s.manager->submit(i, S, 105, 5);
    s.ids = {5, 9, 1, 2, 3};
    return s;
}

Scenario priorityLossScenario() {
    Scenario s{std::make_unique<OrderManager>(), {}};
    for (OrderId i = 1; i <= 4; ++i) s.manager->submit(i, B, 100, 10);
    s.manager->submit(7, S, 120, 4);
    s.ids = {1, 2, 3, 4, 7};
    return s;
}

// Runs `op` under a failure plan, then checks the system at every stage of recovery.
template <typename Op>
long sweepRecovery(const std::string& name, Scenario (*make)(), Op op, OrderId target, int shape) {
    // shape 0 = single failure, 1 = persistent, 2 = pairs
    long points = 0;
    for (long n = 1; n <= 80; ++n) {
        const long maxM = shape == 2 ? n + 10 : n + 1;
        bool anyReached = false;
        for (long m = n + 1; m <= maxM; ++m) {
            Scenario sc = make();
            audit::Auditor auditor;
            if (!auditor.audit(*sc.manager, sc.ids).empty()) { CHECK(false); return points; }
            const std::size_t tradesBefore = sc.manager->trades().size();

            bool threw = false;
            long reached = 0;
            {
                fault::FailAllocation guard(n, shape == 1, shape == 2 ? m : 0);
                try {
                    op(*sc.manager);
                } catch (const std::bad_alloc&) {
                    threw = true;
                }
                reached = guard.allocations();
            }
            if (reached < n) break;     // the first failure point was never reached: done with this n
            anyReached = true;
            ++points;
            if (sc.manager->knows(target)) {
                bool present = false;
                for (OrderId x : sc.ids) present = present || x == target;
                if (!present) sc.ids.push_back(target);
            }
            const std::string ctx = "[" + name + " n=" + std::to_string(n) + " m=" + std::to_string(m) + (threw ? " threw" : "") + "] ";

            // 1. Immediately after the failure: possibly stale, never inconsistent.
            auto v = auditor.audit(*sc.manager, sc.ids);
            if (!v.empty()) { std::cerr << "    " << ctx << "after the operation:" << join(v) << "\n"; CHECK(false); return points; }
            const std::size_t committed = sc.manager->trades().size();
            CHECK_MSG(committed >= tradesBefore, ctx + "trades vanished");

            // 2. Recovery attempts while memory is still exhausted: no throw, no corruption, no change in trades.
            for (int attempt = 0; attempt < 3; ++attempt) {
                { fault::FailAllocation starved(1, true); sc.manager->recover(); }
                v = auditor.audit(*sc.manager, sc.ids);
                if (!v.empty()) { std::cerr << "    " << ctx << "after starved recovery #" << attempt << ":" << join(v) << "\n"; CHECK(false); return points; }
                CHECK_MSG(sc.manager->trades().size() == committed, ctx + "a recovery attempt changed the committed trades");
            }
            // 3. Memory is back: recovery completes, repeatedly and harmlessly.
            CHECK_MSG(sc.manager->recover(), ctx + "recovery did not complete");
            CHECK_MSG(sc.manager->recover(), ctx + "second recovery did not complete");
            CHECK_MSG(sc.manager->isHistoryCurrent(), ctx + "history is still stale after recovery");
            v = auditor.audit(*sc.manager, sc.ids);
            if (!v.empty()) { std::cerr << "    " << ctx << "after recovery:" << join(v) << "\n"; CHECK(false); return points; }
            CHECK_MSG(sc.manager->trades().size() == committed, ctx + "recovery created or lost trades");

            // 4. An accepted order's id is never reusable, whatever happened (a rolled-back
            //    submit never became an order, so its id is legitimately free).
            if (sc.manager->knows(target)) {
                CHECK_MSG(sc.manager->submit(target, B, 100, 1).rejectReason == RejectReason::DuplicateOrderId, ctx + "a recorded id was reusable");
            }
            if (!anyReached) break;
            if (shape != 2) break;
            if (reached < m) break;
        }
        if (!anyReached) return points;
    }
    CHECK(false);  // never stopped allocating
    return points;
}

void runAllShapes(const char* name, Scenario (*make)(), const std::function<void(OrderManager&)>& op, OrderId target,
                  long minPoints) {
    long total = 0;
    for (int shape = 0; shape <= 2; ++shape) {
        total += sweepRecovery(std::string(name) + (shape == 0 ? "/single" : shape == 1 ? "/persistent" : "/double"), make, op, target, shape);
    }
    std::cout << "    [system-fault] " << name << ": " << total << " failure plans checked through recovery\n";
    CHECK_MSG(total >= minPoints, std::string(name) + ": too few failure points were exercised");
}

}  // namespace

TEST(RecoverySweepCrossingSubmitWithRemainder) {
    runAllShapes("crossing-submit", crossingSubmitScenario, [](OrderManager& m) { m.submit(20, B, 100, 28); }, 20, 40);
}

TEST(RecoverySweepReplaceThatCrossesAfterPriorExecutions) {
    runAllShapes("replace-crossing", replaceCrossingScenario, [](OrderManager& m) { m.modify(5, 105, std::nullopt); }, 5, 15);
}

TEST(RecoverySweepQuantityIncreaseLosesPriority) {
    runAllShapes("priority-loss", priorityLossScenario, [](OrderManager& m) { m.modify(1, std::nullopt, 30); }, 1, 3);
}

TEST(RecoverySweepPriceChangeWithoutCrossing) {
    runAllShapes("reprice", priorityLossScenario, [](OrderManager& m) { m.modify(2, 101, 12); }, 2, 3);
}

// ===== Rejected business operations never mutate, even under injection ===================================================

TEST(RejectedBusinessOperationsAllocateNothingAndChangeNothing) {
    Scenario sc = replaceCrossingScenario();
    sc.manager->submit(30, B, 80, 5);
    sc.manager->cancel(30);                 // a cancelled order
    sc.ids.push_back(30);
    audit::Auditor auditor;
    CHECK(auditor.audit(*sc.manager, sc.ids).empty());
    const audit::Snapshot before = audit::take(*sc.manager, sc.ids);

    struct Case { const char* name; std::function<ExecutionReport(OrderManager&)> op; RejectReason why; };
    const std::vector<Case> cases = {
        {"duplicate active id", [](OrderManager& m) { return m.submit(5, B, 90, 1); }, RejectReason::DuplicateOrderId},
        {"duplicate terminal id", [](OrderManager& m) { return m.submit(30, B, 90, 1); }, RejectReason::DuplicateOrderId},
        {"invalid order id", [](OrderManager& m) { return m.submit(0, B, 90, 1); }, RejectReason::InvalidOrderId},
        {"cancel unknown", [](OrderManager& m) { return m.cancel(999); }, RejectReason::OrderNotFound},
        {"cancel cancelled", [](OrderManager& m) { return m.cancel(30); }, RejectReason::OrderNotActive},
        {"cancel filled", [](OrderManager& m) { return m.cancel(9); }, RejectReason::OrderNotActive},
        {"modify unknown", [](OrderManager& m) { return m.modify(999, 90, 1); }, RejectReason::OrderNotFound},
        {"modify terminal", [](OrderManager& m) { return m.modify(30, 90, 1); }, RejectReason::OrderNotActive},
        {"modify no change", [](OrderManager& m) { return m.modify(5, std::nullopt, std::nullopt); }, RejectReason::NoChange},
        {"modify bad price", [](OrderManager& m) { return m.modify(5, 0, std::nullopt); }, RejectReason::InvalidPrice},
        {"modify bad quantity", [](OrderManager& m) { return m.modify(5, std::nullopt, 0); }, RejectReason::InvalidQuantity},
    };
    for (const Case& c : cases) {
        long allocations = -1;
        ExecutionReport report;
        {
            fault::FailAllocation count(0);   // count only
            report = c.op(*sc.manager);
            allocations = count.allocations();
        }
        CHECK_MSG(!report.accepted && report.rejectReason == c.why, std::string(c.name) + ": wrong outcome");
        CHECK_MSG(allocations == 0, std::string(c.name) + ": a rejection allocated, so it could fail with bad_alloc");
        // Same operation with EVERY allocation failing: must behave identically (it allocates nothing).
        {
            fault::FailAllocation hostile(1, true);
            try {
                report = c.op(*sc.manager);
            } catch (const std::bad_alloc&) {
                CHECK_MSG(false, std::string(c.name) + ": threw under injection");
            }
        }
        CHECK_MSG(!report.accepted && report.rejectReason == c.why, std::string(c.name) + ": wrong outcome under injection");
    }
    const audit::Snapshot after = audit::take(*sc.manager, sc.ids);
    CHECK_EQ(after.trades.size(), before.trades.size());
    CHECK_EQ(after.recorded, before.recorded);
    CHECK_EQ(after.bookOrders, before.bookOrders);
    for (std::size_t i = 0; i < before.records.size(); ++i) {
        const auto& a = before.records[i];
        const auto& b = after.records[i];
        CHECK(a.status == b.status && a.executedQuantity == b.executedQuantity && a.remainingQuantity == b.remainingQuantity &&
              a.cancelledQuantity == b.cancelledQuantity && a.price == b.price && a.sequence == b.sequence);
    }
    CHECK(auditor.audit(*sc.manager, sc.ids).empty());
}

// ===== Stale history is healed by every kind of mutating call ============================================================

namespace {

// Seeds a manager with three resting sells and one far bid; the tests then submit a crossing
// order under an injected failure to leave history pending (deferred bookkeeping).
void buildBaseline(OrderManager& m, std::vector<OrderId>& ids) {
    for (OrderId i = 1; i <= 3; ++i) m.submit(i, S, 100, 10);
    m.submit(4, B, 90, 10);
    ids = {1, 2, 3, 4};
}

}  // namespace

TEST(EveryKindOfMutatingCallHealsPendingHistoryBeforeActing) {
    long checked = 0;
    for (int kind = 0; kind < 3; ++kind) {
        for (long n = 1; n <= 40; ++n) {
            OrderManager m;
            std::vector<OrderId> ids;
            buildBaseline(m, ids);
            bool threw = false;
            {
                fault::FailAllocation g(n);
                try { m.submit(5, B, 100, 25); } catch (const std::bad_alloc&) { threw = true; }
            }
            ids.push_back(5);
            if (threw || m.isHistoryCurrent()) continue;     // want: the submit succeeded, history pending
            ++checked;
            audit::Auditor auditor;
            CHECK(auditor.audit(m, ids).empty());             // stale but consistent
            if (kind == 0) m.cancel(4);                        // heals, then cancels
            else if (kind == 1) m.modify(4, 91, 5);            // heals, then replaces
            else { m.submit(6, S, 95, 3); ids.push_back(6); }  // heals, then trades
            CHECK(m.isHistoryCurrent());
            const auto v = auditor.audit(m, ids);
            CHECK_MSG(v.empty(), join(v));
            CHECK_EQ(m.order(5)->tradeIndices.size(), 3u);     // the earlier trades were applied exactly once
        }
    }
    CHECK(checked >= 6);   // each kind met a genuinely stale state
}

TEST(RecoverIsIdempotentAndNeverDuplicatesAnything) {
    for (long n = 1; n <= 40; ++n) {
        OrderManager m;
        std::vector<OrderId> ids;
        buildBaseline(m, ids);
        bool threw = false;
        {
            fault::FailAllocation g(n);
            try { m.submit(5, B, 100, 25); } catch (const std::bad_alloc&) { threw = true; }
        }
        ids.push_back(5);
        if (threw || m.isHistoryCurrent()) continue;
        for (int i = 0; i < 5; ++i) CHECK(m.recover());
        audit::Auditor auditor;
        const auto v = auditor.audit(m, ids);
        CHECK_MSG(v.empty(), join(v));
        std::size_t indices = 0;
        for (OrderId id : ids) indices += m.order(id)->tradeIndices.size();
        CHECK_EQ(indices, 2 * m.trades().size());          // exactly once per side after five recoveries
        CHECK_EQ(m.recordedOrderCount(), 5u);              // no duplicate records
    }
}

TEST_MAIN()
