#pragma once
// The benchmark scenarios. Shared by lob_bench (timing) and lob_alloc_profile (allocation
// counts) so both measure literally the same state construction and the same operations.
//
// Conventions
//  * Setup (building books, generating operation streams) is always untimed.
//  * Operation streams are generated up front, deterministically from fixed seeds, so the
//    timed region contains no random-number generation and no allocation of its own.
//  * Resting (non-crossing) orders: buys at kMid-1-level, sells at kMid+1+level, quantity 10.
//  * "Layer" selects what is exercised: Book = OrderBook::add/cancel only; Engine =
//    MatchingEngine::submit/cancel (adds matching, trade list); Manager = OrderManager
//    (adds id policy, lifecycle records, history bookkeeping). Differences between layers
//    show where cost is added.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "BenchHarness.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "OrderManager.h"
#include "TestRng.h"
#include "WorkloadGenerator.h"

namespace bench {

using lob::OrderId;
using lob::Price;
using lob::Quantity;
using lob::Side;

constexpr Price kMid = 100000;

struct Spec {
    OrderId id;
    Side side;
    Price price;
    Quantity qty;
};

enum class Layer { Book, Engine, Manager };

inline const char* layerName(Layer l) {
    switch (l) {
        case Layer::Book: return "book";
        case Layer::Engine: return "engine";
        case Layer::Manager: return "manager";
    }
    return "?";
}

// Specs for `count` resting, non-crossing orders spread over `levels` price levels per side.
inline std::vector<Spec> restingSpecs(std::size_t count, OrderId firstId, std::size_t levels, std::uint64_t seed,
                                      Quantity qty = 10) {
    TestRng rng(seed);
    std::vector<Spec> v;
    v.reserve(count);
    for (std::size_t k = 0; k < count; ++k) {
        const Price level = static_cast<Price>(rng.below(levels));
        const bool buy = k % 2 == 0;
        v.push_back({firstId + k, buy ? Side::Buy : Side::Sell, buy ? kMid - 1 - level : kMid + 1 + level, qty});
    }
    return v;
}

struct Prepared {
    std::vector<Spec> specs;
    std::vector<lob::Order> orders;  // pre-built Orders for the Book / Engine layers
};

// One of the three stacks, behind a uniform interface.
class Stack {
public:
    explicit Stack(Layer layer) : layer_(layer) { reset(); }

    void reset() {
        book_.reset();
        engine_.reset();
        manager_.reset();
        seq_ = 0;
        switch (layer_) {
            case Layer::Book: book_ = std::make_unique<lob::OrderBook>(); break;
            case Layer::Engine: engine_ = std::make_unique<lob::MatchingEngine>(); break;
            case Layer::Manager: manager_ = std::make_unique<lob::OrderManager>(); break;
        }
    }

    Layer layer() const { return layer_; }

    lob::Order makeOrder(const Spec& s) {
        return *lob::Order::create(s.id, s.side, lob::OrderType::Limit, s.price, s.qty, ++seq_);
    }

    Prepared prepare(std::vector<Spec> specs) {
        Prepared p;
        if (layer_ != Layer::Manager) {
            p.orders.reserve(specs.size());
            for (const Spec& s : specs) p.orders.push_back(makeOrder(s));
        }
        p.specs = std::move(specs);
        return p;
    }

    // Untimed convenience.
    void add(const Spec& s) {
        switch (layer_) {
            case Layer::Book: book_->add(makeOrder(s)); break;
            case Layer::Engine: engine_->submit(makeOrder(s)); break;
            case Layer::Manager: manager_->submit(s.id, s.side, s.price, s.qty); break;
        }
    }

    // Timed path: submit the i-th prepared order.
    void addAt(const Prepared& p, std::size_t i) {
        switch (layer_) {
            case Layer::Book: {
                const lob::RejectReason r = book_->add(p.orders[i]);
                doNotOptimize(r);
                break;
            }
            case Layer::Engine: {
                const lob::ExecutionReport r = engine_->submit(p.orders[i]);
                doNotOptimize(r);
                break;
            }
            case Layer::Manager: {
                const Spec& s = p.specs[i];
                const lob::ExecutionReport r = manager_->submit(s.id, s.side, s.price, s.qty);
                doNotOptimize(r);
                break;
            }
        }
    }

    void cancel(OrderId id) {
        switch (layer_) {
            case Layer::Book: {
                const auto r = book_->cancel(id);
                doNotOptimize(r);
                break;
            }
            case Layer::Engine: {
                const auto r = engine_->cancel(id);
                doNotOptimize(r);
                break;
            }
            case Layer::Manager: {
                const lob::ExecutionReport r = manager_->cancel(id);
                doNotOptimize(r);
                break;
            }
        }
    }

    const lob::OrderBook& book() const {
        if (layer_ == Layer::Book) return *book_;
        if (layer_ == Layer::Engine) return engine_->book();
        return manager_->book();
    }
    lob::OrderManager& manager() { return *manager_; }
    lob::MatchingEngine& engine() { return *engine_; }
    std::size_t trades() const {
        if (layer_ == Layer::Engine) return engine_->trades().size();
        if (layer_ == Layer::Manager) return manager_->trades().size();
        return 0;
    }

    std::size_t active() const { return book().orderCount(); }

    // Untimed structural validation. The manager additionally audits its lifecycle records.
    std::string check() const {
        const lob::InvariantReport b = book().checkInvariants();
        if (!b.ok()) return "book invariant: " + b.violations.front();
        if (layer_ == Layer::Manager) {
            const lob::InvariantReport m = manager_->checkConsistency();
            if (!m.ok()) return "manager consistency: " + m.violations.front();
        }
        return "";
    }

private:
    Layer layer_;
    std::unique_ptr<lob::OrderBook> book_;
    std::unique_ptr<lob::MatchingEngine> engine_;
    std::unique_ptr<lob::OrderManager> manager_;
    lob::Sequence seq_ = 0;
};

inline std::string expect(bool ok, const std::string& what) { return ok ? std::string() : what; }

inline std::string num(std::size_t v) { return std::to_string(v); }

// ---- Submission ---------------------------------------------------------------------------------------------------

struct SubmitState {
    explicit SubmitState(Layer l) : stack(l) {}
    Stack stack;
    Prepared prep;
};

inline Scenario submitResting(Layer layer, std::size_t M, std::size_t L, std::size_t N, const std::string& group) {
    auto st = std::make_shared<SubmitState>(layer);
    Scenario s;
    s.name = "submit_resting";
    s.variant = std::string("layer=") + layerName(layer);
    s.params = "preload=" + num(M) + ";levels=" + num(L) + ";ops=" + num(N);
    s.group = group;
    s.stateOrders = M;
    s.setup = [=]() {
        st->stack.reset();
        for (const Spec& sp : restingSpecs(M, 1, L, 11)) st->stack.add(sp);
        st->prep = st->stack.prepare(restingSpecs(N, M + 1, L, 12));
    };
    s.validateSetup = [=]() { return expect(st->stack.active() == M, "preload count wrong"); };
    s.validate = [=]() {
        std::string e = expect(st->stack.active() == M + N && st->stack.trades() == 0, "orders not all resting / unexpected trades");
        return e.empty() ? st->stack.check() : e;
    };
    bindOps(s, N, [st](std::size_t i) { st->stack.addAt(st->prep, i); });
    return s;
}

// ---- Cancellation ------------------------------------------------------------------------------------------------------

enum class QueueOrder { Oldest, Middle, Newest };
inline const char* queueOrderName(QueueOrder o) {
    return o == QueueOrder::Oldest ? "oldest-first" : o == QueueOrder::Newest ? "newest-first" : "middle-out";
}

struct CancelState {
    explicit CancelState(Layer l) : stack(l) {}
    Stack stack;
    std::vector<OrderId> ids;
};

// All Q orders rest in ONE FIFO queue (one price); they are cancelled in the given order.
inline Scenario cancelInQueue(Layer layer, std::size_t Q, QueueOrder order, const std::string& group) {
    auto st = std::make_shared<CancelState>(layer);
    Scenario s;
    s.name = "cancel_in_queue";
    s.variant = std::string("layer=") + layerName(layer) + ";order=" + queueOrderName(order);
    s.params = "queue=" + num(Q);
    s.group = group;
    s.setup = [=]() {
        st->stack.reset();
        for (std::size_t k = 0; k < Q; ++k) st->stack.add({k + 1, Side::Buy, kMid - 1, 10});
        st->ids.clear();
        if (order == QueueOrder::Oldest) {
            for (std::size_t k = 0; k < Q; ++k) st->ids.push_back(k + 1);
        } else if (order == QueueOrder::Newest) {
            for (std::size_t k = 0; k < Q; ++k) st->ids.push_back(Q - k);
        } else {  // middle-out: m, m+1, m-1, m+2, m-2, ...
            const std::ptrdiff_t m = static_cast<std::ptrdiff_t>(Q / 2);
            st->ids.push_back(static_cast<OrderId>(m + 1));
            for (std::ptrdiff_t d = 1; st->ids.size() < Q; ++d) {
                if (m + d < static_cast<std::ptrdiff_t>(Q)) st->ids.push_back(static_cast<OrderId>(m + d + 1));
                if (st->ids.size() < Q && m - d >= 0) st->ids.push_back(static_cast<OrderId>(m - d + 1));
            }
        }
    };
    s.validate = [=]() { return expect(st->stack.active() == 0, "orders left after cancelling all"); };
    bindOps(s, Q, [st](std::size_t i) { st->stack.cancel(st->ids[i]); });
    return s;
}

// L price levels of Q orders each (sells). Q-1 orders of each of the K BEST levels, or of the K
// WORST levels, are cancelled (the last order of every level stays, so no level is erased and
// only order removal is measured). Compares removal near the front of the ordered price map
// with removal deep inside it.
inline Scenario cancelAtLevel(Layer layer, std::size_t L, std::size_t Q, std::size_t K, bool best, const std::string& group) {
    auto st = std::make_shared<CancelState>(layer);
    Scenario s;
    s.name = "cancel_at_level";
    s.variant = std::string("layer=") + layerName(layer) + ";levels=" + (best ? "best" : "worst");
    s.params = "levels=" + num(L) + ";per_level=" + num(Q) + ";cancel_levels=" + num(K);
    s.group = group;
    s.setup = [=]() {
        st->stack.reset();
        for (std::size_t lvl = 0; lvl < L; ++lvl) {
            for (std::size_t j = 0; j < Q; ++j) {
                st->stack.add({lvl * Q + j + 1, Side::Sell, kMid + 1 + static_cast<Price>(lvl), 10});
            }
        }
        st->ids.clear();
        const std::size_t first = best ? 0 : L - K;
        for (std::size_t lvl = first; lvl < first + K; ++lvl) {
            for (std::size_t j = 0; j + 1 < Q; ++j) st->ids.push_back(lvl * Q + j + 1);
        }
    };
    s.validate = [=]() {
        return expect(st->stack.active() == L * Q - K * (Q - 1) && st->stack.book().askLevelCount() == L, "unexpected book after cancels");
    };
    bindOps(s, K * (Q - 1), [st](std::size_t i) { st->stack.cancel(st->ids[i]); });
    return s;
}

// Preload M resting orders over L levels; cancel N distinct random ones.
inline Scenario cancelRandom(Layer layer, std::size_t M, std::size_t L, std::size_t N, const std::string& group) {
    auto st = std::make_shared<CancelState>(layer);
    Scenario s;
    s.name = "cancel_random";
    s.variant = std::string("layer=") + layerName(layer);
    s.params = "preload=" + num(M) + ";levels=" + num(L) + ";ops=" + num(N);
    s.group = group;
    s.stateOrders = M;
    s.setup = [=]() {
        st->stack.reset();
        for (const Spec& sp : restingSpecs(M, 1, L, 21)) st->stack.add(sp);
        std::vector<OrderId> all(M);
        for (std::size_t k = 0; k < M; ++k) all[k] = k + 1;
        TestRng rng(22);
        for (std::size_t k = M; k > 1; --k) std::swap(all[k - 1], all[rng.below(k)]);  // shuffle
        st->ids.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(N));
    };
    s.validateSetup = [=]() { return expect(st->stack.active() == M, "preload count wrong"); };
    s.validate = [=]() {
        std::string e = expect(st->stack.active() == M - N, "wrong number of orders left");
        return e.empty() ? st->stack.check() : e;
    };
    bindOps(s, N, [st](std::size_t i) { st->stack.cancel(st->ids[i]); });
    return s;
}

// ---- Modification (manager layer: the public API for it) ---------------------------------------------------------------------

enum class ModKind { QtyDecrease, QtyIncrease, PriceChange };
inline const char* modKindName(ModKind k) {
    return k == ModKind::QtyDecrease ? "qty-decrease(in-place)" : k == ModKind::QtyIncrease ? "qty-increase(replace)" : "price-change(replace)";
}

struct ModifyState {
    std::unique_ptr<lob::OrderManager> mgr;
    std::vector<Spec> targets;   // the orders to modify, with their ORIGINAL terms
};

inline Scenario modifyOrders(ModKind kind, std::size_t M, std::size_t L, std::size_t N, const std::string& group) {
    auto st = std::make_shared<ModifyState>();
    Scenario s;
    s.name = "modify";
    s.variant = std::string("kind=") + modKindName(kind);
    s.params = "preload=" + num(M) + ";levels=" + num(L) + ";ops=" + num(N);
    s.group = group;
    s.stateOrders = M;
    s.setup = [=]() {
        st->mgr = std::make_unique<lob::OrderManager>();
        const std::vector<Spec> specs = restingSpecs(M, 1, L, 31);
        for (const Spec& sp : specs) st->mgr->submit(sp.id, sp.side, sp.price, sp.qty);
        std::vector<std::size_t> pick(M);
        for (std::size_t k = 0; k < M; ++k) pick[k] = k;
        TestRng rng(32);
        for (std::size_t k = M; k > 1; --k) std::swap(pick[k - 1], pick[rng.below(k)]);
        st->targets.clear();
        for (std::size_t k = 0; k < N; ++k) st->targets.push_back(specs[pick[k]]);
    };
    s.validateSetup = [=]() { return expect(st->mgr->activeOrderCount() == M, "preload count wrong"); };
    s.validate = [=]() {
        for (const Spec& t : st->targets) {  // every targeted order carries its new terms; nothing traded
            const auto rec = st->mgr->order(t.id);
            if (!rec || !rec->isActive()) return std::string("a modified order is not active");
            const bool ok = kind == ModKind::QtyDecrease ? rec->remainingQuantity == 5
                            : kind == ModKind::QtyIncrease ? rec->remainingQuantity == 20
                                                           : rec->price == (t.side == Side::Buy ? t.price - 1 : t.price + 1);
            if (!ok) return std::string("a modification did not take effect");
        }
        return expect(st->mgr->trades().empty(), "unexpected trades");
    };
    bindOps(s, N, [st, kind](std::size_t i) {
        const Spec& t = st->targets[i];
        std::optional<Price> p;
        std::optional<Quantity> q;
        if (kind == ModKind::QtyDecrease) q = 5;
        else if (kind == ModKind::QtyIncrease) q = 20;
        else p = t.side == Side::Buy ? t.price - 1 : t.price + 1;
        const lob::ExecutionReport r = st->mgr->modify(t.id, p, q);
        doNotOptimize(r);
    });
    return s;
}

// ---- Matching -------------------------------------------------------------------------------------------------------------

// N resting sells (qty 1) at one price; N incoming buys (qty 1): each incoming order trades
// with exactly one resting order. ops = incoming orders (= trades).
inline Scenario matchOneToOne(Layer layer, std::size_t N, const std::string& group) {
    auto st = std::make_shared<SubmitState>(layer);
    Scenario s;
    s.name = "match_one_to_one";
    s.variant = std::string("layer=") + layerName(layer);
    s.params = "trades=" + num(N);
    s.group = group;
    s.setup = [=]() {
        st->stack.reset();
        for (std::size_t k = 0; k < N; ++k) st->stack.add({k + 1, Side::Sell, kMid, 1});
        std::vector<Spec> in;
        for (std::size_t k = 0; k < N; ++k) in.push_back({N + k + 1, Side::Buy, kMid, 1});
        st->prep = st->stack.prepare(std::move(in));
    };
    s.validate = [=]() {
        std::string e = expect(st->stack.trades() == N && st->stack.active() == 0, "expected exactly N trades and an empty book");
        return e.empty() ? st->stack.check() : e;
    };
    bindOps(s, N, [st](std::size_t i) { st->stack.addAt(st->prep, i); });
    return s;
}

// ONE incoming buy sweeps M resting sells (qty 1) at one price: M trades from a single call.
inline Scenario matchOneToMany(Layer layer, std::size_t M, const std::string& group) {
    auto st = std::make_shared<SubmitState>(layer);
    Scenario s;
    s.name = "match_one_to_many";
    s.variant = std::string("layer=") + layerName(layer);
    s.params = "trades=" + num(M) + ";makers=" + num(M) + ";levels=1";
    s.group = group;
    s.opsPerRun = M;  // operations = trades
    s.setup = [=]() {
        st->stack.reset();
        for (std::size_t k = 0; k < M; ++k) st->stack.add({k + 1, Side::Sell, kMid, 1});
        st->prep = st->stack.prepare({{M + 1, Side::Buy, kMid, static_cast<Quantity>(M)}});
    };
    s.timed = [st]() { st->stack.addAt(st->prep, 0); };
    s.validate = [=]() {
        std::string e = expect(st->stack.trades() == M && st->stack.active() == 0, "expected M trades and an empty book");
        return e.empty() ? st->stack.check() : e;
    };
    return s;
}

// One big resting sell (qty N) hit by N incoming buys (qty 1): N partial fills of the same maker.
inline Scenario matchManyToOne(Layer layer, std::size_t N, const std::string& group) {
    auto st = std::make_shared<SubmitState>(layer);
    Scenario s;
    s.name = "match_many_to_one";
    s.variant = std::string("layer=") + layerName(layer);
    s.params = "trades=" + num(N);
    s.group = group;
    s.setup = [=]() {
        st->stack.reset();
        st->stack.add({1, Side::Sell, kMid, static_cast<Quantity>(N)});
        std::vector<Spec> in;
        for (std::size_t k = 0; k < N; ++k) in.push_back({k + 2, Side::Buy, kMid, 1});
        st->prep = st->stack.prepare(std::move(in));
    };
    s.validate = [=]() {
        std::string e = expect(st->stack.trades() == N && st->stack.active() == 0, "expected N trades and an empty book");
        return e.empty() ? st->stack.check() : e;
    };
    bindOps(s, N, [st](std::size_t i) { st->stack.addAt(st->prep, i); });
    return s;
}

// L price levels with Q orders each (qty 1); ONE incoming buy sweeps all of them: L*Q trades,
// L level erasures.
inline Scenario matchMultiLevel(Layer layer, std::size_t L, std::size_t Q, const std::string& group) {
    auto st = std::make_shared<SubmitState>(layer);
    Scenario s;
    s.name = "match_multi_level";
    s.variant = std::string("layer=") + layerName(layer);
    s.params = "levels=" + num(L) + ";per_level=" + num(Q) + ";trades=" + num(L * Q);
    s.group = group;
    s.opsPerRun = L * Q;
    s.setup = [=]() {
        st->stack.reset();
        OrderId id = 1;
        for (std::size_t lvl = 0; lvl < L; ++lvl) {
            for (std::size_t j = 0; j < Q; ++j) st->stack.add({id++, Side::Sell, kMid + static_cast<Price>(lvl), 1});
        }
        st->prep = st->stack.prepare({{id, Side::Buy, kMid + static_cast<Price>(L), static_cast<Quantity>(L * Q)}});
    };
    s.timed = [st]() { st->stack.addAt(st->prep, 0); };
    s.validate = [=]() {
        std::string e = expect(st->stack.trades() == L * Q && st->stack.active() == 0, "expected L*Q trades and an empty book");
        return e.empty() ? st->stack.check() : e;
    };
    return s;
}

// ---- Read-only queries -----------------------------------------------------------------------------------------------------------

struct BookQueryState {
    std::unique_ptr<lob::OrderManager> mgr;
    std::vector<OrderId> probe;  // ids to look up, in random order
};

inline std::shared_ptr<BookQueryState> buildQueryState(std::size_t M, std::size_t L, std::size_t probes) {
    auto st = std::make_shared<BookQueryState>();
    st->mgr = std::make_unique<lob::OrderManager>();
    for (const Spec& sp : restingSpecs(M, 1, L, 41)) st->mgr->submit(sp.id, sp.side, sp.price, sp.qty);
    TestRng rng(42);
    st->probe.reserve(probes);
    for (std::size_t k = 0; k < probes; ++k) st->probe.push_back(1 + rng.below(M));
    return st;
}

// Holds a query state shared by the setup and the operations (captured by value).
struct QueryHolder {
    std::shared_ptr<BookQueryState> st;
};

inline Scenario marketData(const std::string& which, std::size_t M, std::size_t L, std::size_t N, const std::string& group) {
    auto h = std::make_shared<QueryHolder>();
    Scenario s;
    s.name = "market_data";
    s.variant = "query=" + which;
    s.params = "orders=" + num(M) + ";levels_per_side=" + num(L) + ";ops=" + num(N);
    s.group = group;
    s.stateOrders = M;
    s.readOnly = true;
    s.setup = [=]() { h->st = buildQueryState(M, L, 16); };
    s.validateSetup = [=]() { return expect(h->st && h->st->mgr->activeOrderCount() == M, "book not built"); };
    // The queries are non-inlined calls into the library, so they are not hoisted out of the loop.
    if (which == "bestBid") {
        bindOps(s, N, [h](std::size_t) { const auto r = h->st->mgr->book().bestBid(); doNotOptimize(r); });
    } else if (which == "bestAsk") {
        bindOps(s, N, [h](std::size_t) { const auto r = h->st->mgr->book().bestAsk(); doNotOptimize(r); });
    } else if (which == "spread") {
        bindOps(s, N, [h](std::size_t) { const auto r = h->st->mgr->book().spread(); doNotOptimize(r); });
    } else if (which == "midPrice") {
        bindOps(s, N, [h](std::size_t) { const auto r = h->st->mgr->book().midPrice(); doNotOptimize(r); });
    } else {  // depth(n)
        const std::size_t n = static_cast<std::size_t>(std::stoul(which.substr(which.find('(') + 1)));
        bindOps(s, N, [h, n](std::size_t) { const auto r = h->st->mgr->book().depth(n); doNotOptimize(r); });
    }
    return s;
}

inline Scenario lookup(const std::string& which, std::size_t M, std::size_t L, std::size_t N, const std::string& group) {
    auto h = std::make_shared<QueryHolder>();
    Scenario s;
    s.name = "order_lookup";
    s.variant = "query=" + which;
    s.params = "orders=" + num(M) + ";levels_per_side=" + num(L) + ";ops=" + num(N);
    s.group = group;
    s.stateOrders = M;
    s.readOnly = true;
    s.setup = [=]() { h->st = buildQueryState(M, L, N); };
    s.validateSetup = [=]() { return expect(h->st && h->st->mgr->activeOrderCount() == M, "book not built"); };
    if (which == "book.find") {
        bindOps(s, N, [h](std::size_t i) { const lob::Order* o = h->st->mgr->book().find(h->st->probe[i]); doNotOptimize(o); });
    } else if (which == "book.contains") {
        bindOps(s, N, [h](std::size_t i) { const bool b = h->st->mgr->book().contains(h->st->probe[i]); doNotOptimize(b); });
    } else if (which == "manager.knows") {
        bindOps(s, N, [h](std::size_t i) { const bool b = h->st->mgr->knows(h->st->probe[i]); doNotOptimize(b); });
    } else {  // manager.order (copies the lifecycle record)
        bindOps(s, N, [h](std::size_t i) { const auto r = h->st->mgr->order(h->st->probe[i]); doNotOptimize(r); });
    }
    return s;
}

// ---- Operation streams (mixed workloads) ----------------------------------------------------------------------------------------

struct Op {
    enum Kind : std::uint8_t { SubmitGen, SubmitId, Cancel, Modify } kind = SubmitGen;
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Quantity qty = 0;
    bool hasPrice = false, hasQty = false;
    Price newPrice = 0;
    Quantity newQty = 0;
};

inline const char* opKindName(Op::Kind k) {
    switch (k) {
        case Op::SubmitGen: return "submit";
        case Op::SubmitId: return "submit";
        case Op::Cancel: return "cancel";
        case Op::Modify: return "modify";
    }
    return "?";
}

inline std::uint64_t digestOf(const lob::OrderManager& m) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    for (const lob::Trade& t : m.trades()) {
        mix(t.id());
        mix(t.makerOrderId());
        mix(t.takerOrderId());
        mix(static_cast<std::uint64_t>(t.price()));
        mix(static_cast<std::uint64_t>(t.quantity()));
    }
    mix(m.recordedOrderCount());
    mix(m.activeOrderCount());
    return h;
}

// The Phase 7 generator's target for the benchmarks: it executes each operation on a scratch
// OrderManager (so the generator can see which orders are live) and RECORDS the operation, to
// be replayed on a fresh manager in the timed region.
class Recorder {
public:
    lob::ExecutionReport submit(Side side, Price price, Quantity qty) {
        Op op;
        op.kind = Op::SubmitGen;
        op.side = side;
        op.price = price;
        op.qty = qty;
        return apply(op, mgr_.submit(side, price, qty));
    }
    lob::ExecutionReport submit(OrderId id, Side side, Price price, Quantity qty) {
        Op op;
        op.kind = Op::SubmitId;
        op.id = id;
        op.side = side;
        op.price = price;
        op.qty = qty;
        return apply(op, mgr_.submit(id, side, price, qty));
    }
    lob::ExecutionReport cancel(OrderId id) {
        Op op;
        op.kind = Op::Cancel;
        op.id = id;
        return apply(op, mgr_.cancel(id));
    }
    lob::ExecutionReport modify(OrderId id, std::optional<Price> p, std::optional<Quantity> q) {
        Op op;
        op.kind = Op::Modify;
        op.id = id;
        op.hasPrice = p.has_value();
        op.hasQty = q.has_value();
        op.newPrice = p.value_or(0);
        op.newQty = q.value_or(0);
        return apply(op, mgr_.modify(id, p, q));
    }
    const std::vector<OrderId>& knownIds() const { return ids_; }
    bool isActive(OrderId id) const { return mgr_.book().find(id) != nullptr; }
    bool healthy() const { return true; }

    std::vector<Op> ops;
    lob::OrderManager& manager() { return mgr_; }

private:
    lob::ExecutionReport apply(const Op& op, lob::ExecutionReport report) {
        ops.push_back(op);
        if (mgr_.recordedOrderCount() > ids_.size()) ids_.push_back(report.orderId);  // a new record was made
        return report;
    }
    lob::OrderManager mgr_;
    std::vector<OrderId> ids_;
};

struct Stream {
    std::vector<Op> ops;
    std::vector<Op> preload;     // applied untimed before the stream (synthetic streams)
    std::size_t expectTrades = 0;
    std::size_t expectRecorded = 0;
    std::size_t expectActive = 0;
    std::uint64_t expectDigest = 0;
};

// Replays an operation on a manager; returns the report's accepted flag (consumed to keep the call alive).
inline void replay(lob::OrderManager& m, const Op& op) {
    switch (op.kind) {
        case Op::SubmitGen: {
            const lob::ExecutionReport r = m.submit(op.side, op.price, op.qty);
            doNotOptimize(r);
            break;
        }
        case Op::SubmitId: {
            const lob::ExecutionReport r = m.submit(op.id, op.side, op.price, op.qty);
            doNotOptimize(r);
            break;
        }
        case Op::Cancel: {
            const lob::ExecutionReport r = m.cancel(op.id);
            doNotOptimize(r);
            break;
        }
        case Op::Modify: {
            std::optional<Price> p;
            std::optional<Quantity> q;
            if (op.hasPrice) p = op.newPrice;
            if (op.hasQty) q = op.newQty;
            const lob::ExecutionReport r = m.modify(op.id, p, q);
            doNotOptimize(r);
            break;
        }
    }
}

struct ReplayState {
    std::unique_ptr<lob::OrderManager> mgr;
    Stream stream;
};

inline std::string validateReplay(const ReplayState& st) {
    if (st.mgr->trades().size() != st.stream.expectTrades) return "trade count differs from the generation pass";
    if (st.mgr->recordedOrderCount() != st.stream.expectRecorded) return "record count differs from the generation pass";
    if (st.mgr->activeOrderCount() != st.stream.expectActive) return "active count differs from the generation pass";
    if (digestOf(*st.mgr) != st.stream.expectDigest) return "trade-stream digest differs from the generation pass";
    const lob::InvariantReport c = st.mgr->checkConsistency();
    return c.ok() ? std::string() : "manager consistency: " + c.violations.front();
}

inline Scenario replayScenario(const std::string& name, const std::string& variant, const std::string& params,
                               const std::string& group, std::shared_ptr<Stream> stream, bool perOpByKind) {
    auto st = std::make_shared<ReplayState>();
    Scenario s;
    s.name = name;
    s.variant = variant;
    s.params = params;
    s.group = group;
    s.perOpOnly = perOpByKind;
    s.setup = [=]() {
        st->mgr = std::make_unique<lob::OrderManager>();
        st->stream.expectTrades = stream->expectTrades;
        st->stream.expectRecorded = stream->expectRecorded;
        st->stream.expectActive = stream->expectActive;
        st->stream.expectDigest = stream->expectDigest;
        for (const Op& op : stream->preload) replay(*st->mgr, op);
    };
    s.validate = [=]() { return validateReplay(*st); };
    s.note = [st]() {
        return "trades=" + num(st->mgr->trades().size()) + ";records=" + num(st->mgr->recordedOrderCount()) + ";active=" + num(st->mgr->activeOrderCount());
    };
    const std::size_t n = stream->ops.size();
    bindOps(s, n, [st, stream](std::size_t i) { replay(*st->mgr, stream->ops[i]); });
    if (perOpByKind) {
        // Latency grouped by operation kind (per-op timing only).
        s.latency = [st, stream, n](std::size_t, std::vector<LatencyGroup>& out) {
            LatencyGroup submit, cancel, modify;
            submit.name = "submit";
            cancel.name = "cancel";
            modify.name = "modify";
            submit.method = cancel.method = modify.method = "per_op";
            for (std::size_t i = 0; i < n; ++i) {
                const Op& op = stream->ops[i];
                const std::int64_t t0 = nowNs();
                replay(*st->mgr, op);
                const std::int64_t t1 = nowNs();
                LatencyGroup& g = op.kind == Op::Cancel ? cancel : op.kind == Op::Modify ? modify : submit;
                g.samplesNs.push_back(static_cast<double>(t1 - t0));
            }
            out.push_back(std::move(submit));
            out.push_back(std::move(cancel));
            out.push_back(std::move(modify));
        };
    }
    return s;
}

// A Phase 7 profile: generate with the shared generator, record, then replay.
inline Scenario mixedWorkload(const workload::Profile& profile, std::uint64_t seed, int steps, const std::string& group) {
    auto rec = std::make_unique<Recorder>();
    workload::Generator<Recorder> gen(*rec, profile, seed);
    gen.run(steps);
    auto stream = std::make_shared<Stream>();
    stream->ops = rec->ops;
    stream->expectTrades = rec->manager().trades().size();
    stream->expectRecorded = rec->manager().recordedOrderCount();
    stream->expectActive = rec->manager().activeOrderCount();
    stream->expectDigest = digestOf(rec->manager());
    return replayScenario("mixed_phase7", std::string("profile=") + profile.name, "seed=" + std::to_string(seed) + ";steps=" + num(static_cast<std::size_t>(steps)) +
                                                                                      ";ops=" + num(stream->ops.size()),
                          group, stream, true);
}

// Synthetic stream with a controlled share of aggressive (crossing) orders.
inline Scenario matchingIntensity(unsigned crossPct, std::size_t N, std::size_t background, const std::string& group) {
    auto stream = std::make_shared<Stream>();
    TestRng rng(5000 + crossPct);
    OrderId nextId = 1;
    auto resting = [&](Side side) {
        const Price lvl = static_cast<Price>(rng.below(20));
        Op op;
        op.kind = Op::SubmitId;
        op.id = nextId++;
        op.side = side;
        op.price = side == Side::Buy ? kMid - 1 - lvl : kMid + 1 + lvl;
        op.qty = 5;
        return op;
    };
    for (std::size_t k = 0; k < background; ++k) stream->preload.push_back(resting(k % 2 == 0 ? Side::Buy : Side::Sell));
    for (std::size_t k = 0; k < N; ++k) {
        if (rng.chance(crossPct)) {  // aggressive: crosses up to ~5 levels of the opposite side
            Op op;
            op.kind = Op::SubmitId;
            op.id = nextId++;
            op.side = rng.chance(50) ? Side::Buy : Side::Sell;
            op.price = op.side == Side::Buy ? kMid + 5 : kMid - 5;
            op.qty = static_cast<Quantity>(1 + rng.below(12));
            stream->ops.push_back(op);
        } else {
            stream->ops.push_back(resting(rng.chance(50) ? Side::Buy : Side::Sell));
        }
    }
    lob::OrderManager scratch;  // derive the expected end state for validation
    for (const Op& op : stream->preload) replay(scratch, op);
    for (const Op& op : stream->ops) replay(scratch, op);
    stream->expectTrades = scratch.trades().size();
    stream->expectRecorded = scratch.recordedOrderCount();
    stream->expectActive = scratch.activeOrderCount();
    stream->expectDigest = digestOf(scratch);
    return replayScenario("matching_intensity", "cross_pct=" + std::to_string(crossPct), "ops=" + num(N) + ";background=" + num(background), group, stream, false);
}

// Synthetic stream with controlled cancel and modify frequencies over live resting orders.
inline Scenario cancelModifyFrequency(unsigned cancelPct, unsigned modifyPct, std::size_t N, std::size_t background, const std::string& group) {
    auto stream = std::make_shared<Stream>();
    TestRng rng(7000 + cancelPct * 100 + modifyPct);
    OrderId nextId = 1;
    std::vector<Op> live;  // the resting orders (with their current terms)
    auto resting = [&]() {
        const bool buy = rng.chance(50);
        const Price lvl = static_cast<Price>(rng.below(200));
        Op op;
        op.kind = Op::SubmitId;
        op.id = nextId++;
        op.side = buy ? Side::Buy : Side::Sell;
        op.price = buy ? kMid - 1 - lvl : kMid + 1 + lvl;
        op.qty = 10;
        return op;
    };
    for (std::size_t k = 0; k < background; ++k) {
        stream->preload.push_back(resting());
        live.push_back(stream->preload.back());
    }
    for (std::size_t k = 0; k < N; ++k) {
        const std::uint64_t r = rng.below(100);
        if (r < cancelPct && !live.empty()) {
            const std::size_t i = rng.below(live.size());
            Op op;
            op.kind = Op::Cancel;
            op.id = live[i].id;
            stream->ops.push_back(op);
            live[i] = live.back();
            live.pop_back();
        } else if (r < cancelPct + modifyPct && !live.empty()) {
            const std::size_t i = rng.below(live.size());
            Op op;
            op.kind = Op::Modify;
            op.id = live[i].id;
            const std::uint64_t kind = rng.below(3);
            if (kind == 0) {  // quantity decrease (in place)
                if (live[i].qty <= 1) { op.hasQty = true; op.newQty = live[i].qty + 1; live[i].qty += 1; }
                else { op.hasQty = true; op.newQty = live[i].qty - 1; live[i].qty -= 1; }
            } else if (kind == 1) {  // quantity increase (replace)
                op.hasQty = true;
                op.newQty = live[i].qty + 3;
                live[i].qty += 3;
            } else {  // price change away from the touch (replace), never crossing
                op.hasPrice = true;
                op.newPrice = live[i].side == Side::Buy ? live[i].price - 1 : live[i].price + 1;
                live[i].price = op.newPrice;
            }
            stream->ops.push_back(op);
        } else {
            Op op = resting();
            stream->ops.push_back(op);
            live.push_back(op);
        }
    }
    lob::OrderManager scratch;
    for (const Op& op : stream->preload) replay(scratch, op);
    for (const Op& op : stream->ops) replay(scratch, op);
    stream->expectTrades = scratch.trades().size();
    stream->expectRecorded = scratch.recordedOrderCount();
    stream->expectActive = scratch.activeOrderCount();
    stream->expectDigest = digestOf(scratch);
    return replayScenario("cancel_modify_frequency", "cancel_pct=" + std::to_string(cancelPct) + ";modify_pct=" + std::to_string(modifyPct),
                          "ops=" + num(N) + ";background=" + num(background), group, stream, false);
}

// ---- The registry -------------------------------------------------------------------------------------------------------------------

struct Sizes {
    double divisor = 1.0;  // quick mode divides every count
    std::size_t sz(std::size_t full, std::size_t floor = 64) const {
        return std::max(floor, static_cast<std::size_t>(static_cast<double>(full) / divisor));
    }
};

inline std::vector<Scenario> buildScenarios(const Sizes& z) {
    std::vector<Scenario> v;
    auto add = [&v](Scenario s) { v.push_back(std::move(s)); };
    const Layer all[] = {Layer::Book, Layer::Engine, Layer::Manager};
    const Layer eng[] = {Layer::Engine, Layer::Manager};
    const Layer bm[] = {Layer::Book, Layer::Manager};

    // ---------------- core ----------------
    for (Layer l : all) add(submitResting(l, z.sz(100000), z.sz(1000, 8), z.sz(50000), "core"));
    for (Layer l : all) {
        for (QueueOrder o : {QueueOrder::Oldest, QueueOrder::Middle, QueueOrder::Newest}) add(cancelInQueue(l, z.sz(50000), o, "core"));
    }
    for (Layer l : bm) {
        add(cancelAtLevel(l, z.sz(10000, 40), 20, z.sz(1000, 4), true, "core"));
        add(cancelAtLevel(l, z.sz(10000, 40), 20, z.sz(1000, 4), false, "core"));
    }
    for (Layer l : all) add(cancelRandom(l, z.sz(100000), z.sz(1000, 8), z.sz(20000), "core"));
    for (ModKind k : {ModKind::QtyDecrease, ModKind::QtyIncrease, ModKind::PriceChange}) add(modifyOrders(k, z.sz(100000), z.sz(1000, 8), z.sz(20000), "core"));
    for (Layer l : eng) {
        add(matchOneToOne(l, z.sz(50000), "core"));
        add(matchOneToMany(l, z.sz(50000), "core"));
        add(matchManyToOne(l, z.sz(50000), "core"));
        add(matchMultiLevel(l, z.sz(5000), 1, "core"));
        add(matchMultiLevel(l, z.sz(100, 4), z.sz(500, 4), "core"));
    }
    for (const char* q : {"bestBid", "bestAsk", "spread", "midPrice"}) add(marketData(q, z.sz(100000), z.sz(1000, 8), z.sz(2000000), "core"));
    for (const char* q : {"depth(1)", "depth(5)", "depth(20)", "depth(100)"}) add(marketData(q, z.sz(100000), z.sz(1000, 8), z.sz(50000), "core"));
    for (const char* q : {"book.find", "book.contains", "manager.knows", "manager.order"}) add(lookup(q, z.sz(100000), z.sz(1000, 8), z.sz(500000), "core"));
    for (const workload::Profile* p : {&workload::kNarrow, &workload::kWide, &workload::kMixed}) add(mixedWorkload(*p, 20260601, static_cast<int>(z.sz(30000, 200)), "core"));

    // ---------------- scaling: active orders (10 orders per level) ----------------
    for (std::size_t M : {1000u, 10000u, 100000u, 250000u}) {
        const std::size_t mm = z.sz(M, 100);
        const std::size_t ll = std::max<std::size_t>(mm / 10, 4);
        for (Layer l : bm) add(submitResting(l, mm, ll, z.sz(20000), "scale-active"));
        for (Layer l : bm) add(cancelRandom(l, mm, ll, std::min<std::size_t>(z.sz(20000), mm / 2), "scale-active"));
        add(lookup("book.find", mm, ll, z.sz(500000), "scale-active"));
        add(lookup("manager.order", mm, ll, z.sz(500000), "scale-active"));
        add(marketData("bestBid", mm, ll, z.sz(2000000), "scale-active"));
        add(marketData("depth(10)", mm, ll, z.sz(50000), "scale-active"));
    }
    // ---------------- scaling: number of price levels (100k orders) ----------------
    for (std::size_t L : {1u, 10u, 100u, 1000u, 10000u, 100000u}) {
        const std::size_t M = z.sz(100000, 200);
        const std::size_t ll = std::max<std::size_t>(std::min<std::size_t>(L, M / 2), 1);
        for (Layer l : bm) add(submitResting(l, M, ll, z.sz(20000), "scale-levels"));
        for (Layer l : bm) add(cancelRandom(l, M, ll, z.sz(20000), "scale-levels"));
        add(lookup("book.find", M, ll, z.sz(500000), "scale-levels"));
        add(marketData("bestBid", M, ll, z.sz(2000000), "scale-levels"));
        add(marketData("depth(10)", M, ll, z.sz(50000), "scale-levels"));
    }
    // ---------------- scaling: orders per price level ----------------
    // One FIFO queue of Q orders at a single price (cancel in queue order), and sweeps of
    // 100 levels holding Q orders each.
    for (std::size_t Q : {100u, 1000u, 10000u, 50000u}) {
        const std::size_t QQ = z.sz(Q, 16);
        for (Layer l : bm) {
            add(cancelInQueue(l, QQ, QueueOrder::Oldest, "scale-perlevel"));
            add(cancelInQueue(l, QQ, QueueOrder::Newest, "scale-perlevel"));
        }
    }
    for (std::size_t Q : {10u, 100u, 500u}) {
        for (Layer l : eng) add(matchMultiLevel(l, z.sz(100, 4), z.sz(Q, 4), "scale-perlevel"));
    }
    // ---------------- scaling: matching intensity ----------------
    for (unsigned c : {0u, 10u, 30u, 50u, 70u, 90u}) add(matchingIntensity(c, z.sz(100000), z.sz(4000, 100), "scale-matching"));
    // ---------------- scaling: cancel / modify frequency ----------------
    const unsigned freq[][2] = {{0, 0}, {10, 0}, {30, 0}, {50, 0}, {0, 10}, {0, 30}, {0, 50}, {20, 20}, {35, 35}};
    for (const auto& f : freq) add(cancelModifyFrequency(f[0], f[1], z.sz(100000), z.sz(50000), "scale-cancelmodify"));
    return v;
}

}  // namespace bench
