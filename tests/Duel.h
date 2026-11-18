#pragma once
// Test-only harness that runs the SAME operations through the production
// OrderManager and the independent reference model (ReferenceExchange.h), compares
// everything observable after each operation, and runs the system auditor.
//
// On the first mismatch it prints a reproduction (label, seed, operation number,
// the operation, and expected vs actual) to stderr, records it, and stops comparing,
// so one root cause does not produce thousands of lines. Tests then CHECK(duel.clean()).
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "OrderManager.h"
#include "ReferenceExchange.h"
#include "SystemAuditor.h"

class Duel {
public:
    // auditEvery: run the full audit + full state comparison every N operations
    // (1 = after every operation). The touched orders and the book are compared after
    // every operation regardless.
    explicit Duel(std::string label, std::uint64_t seed = 0, int auditEvery = 1, lob::OrderId firstGenerated = 1,
                  lob::TradeId firstTrade = 1)
        : manager(firstGenerated, firstTrade),
          model(firstGenerated, firstTrade),
          label_(std::move(label)),
          seed_(seed),
          auditEvery_(auditEvery < 1 ? 1 : auditEvery) {}

    lob::OrderManager manager;
    ref::Exchange model;
    std::vector<lob::OrderId> ids;  // every id the model has recorded, in first-seen order

    bool clean() const { return failure_.empty(); }

    // The interface workload::Generator (WorkloadGenerator.h) needs from a target.
    const std::vector<lob::OrderId>& knownIds() const { return ids; }
    bool isActive(lob::OrderId id) const {
        const auto it = model.orders().find(id);
        return it != model.orders().end() && it->second.active();
    }
    bool healthy() const { return clean(); }
    const std::string& failure() const { return failure_; }
    int operations() const { return ops_; }

    // ---- Operations ------------------------------------------------------------------------

    lob::ExecutionReport submit(lob::Side side, lob::Price price, lob::Quantity qty) {
        std::ostringstream d;
        d << "submit(generated id) " << lob::toString(side) << " price=" << price << " qty=" << qty;
        const std::size_t before = model.orders().size();
        const ref::Expected want = model.submitGenerated(side, price, qty);
        const lob::ExecutionReport got = manager.submit(side, price, qty);
        return finish(d.str(), want, got, before);
    }

    lob::ExecutionReport submit(lob::OrderId id, lob::Side side, lob::Price price, lob::Quantity qty) {
        std::ostringstream d;
        d << "submit(id=" << id << ") " << lob::toString(side) << " price=" << price << " qty=" << qty;
        const std::size_t before = model.orders().size();
        const ref::Expected want = model.submit(id, side, price, qty);
        const lob::ExecutionReport got = manager.submit(id, side, price, qty);
        return finish(d.str(), want, got, before);
    }

    lob::ExecutionReport cancel(lob::OrderId id) {
        std::ostringstream d;
        d << "cancel(id=" << id << ")";
        const std::size_t before = model.orders().size();
        const ref::Expected want = model.cancel(id);
        const lob::ExecutionReport got = manager.cancel(id);
        return finish(d.str(), want, got, before);
    }

    lob::ExecutionReport modify(lob::OrderId id, std::optional<lob::Price> price, std::optional<lob::Quantity> qty) {
        std::ostringstream d;
        d << "modify(id=" << id << ") price=";
        if (price) d << *price; else d << "-";
        d << " qty=";
        if (qty) d << *qty; else d << "-";
        const std::size_t before = model.orders().size();
        const ref::Expected want = model.modify(id, price, qty);
        const lob::ExecutionReport got = manager.modify(id, price, qty);
        return finish(d.str(), want, got, before);
    }

    // Full comparison + audit on demand (the final check of a test).
    void checkEverything(const std::string& why = "final check") {
        if (!clean()) return;
        fullCompare(why);
        fullAudit(why);
    }

private:
    static std::string tradesText(const std::vector<ref::Trade>& ts) {
        std::ostringstream o;
        for (const auto& t : ts) o << " {maker " << t.maker << " taker " << t.taker << " @" << t.price << " x" << t.quantity << "}";
        return ts.empty() ? " none" : o.str();
    }
    static std::string tradesText(const std::vector<lob::Trade>& ts) {
        std::ostringstream o;
        for (const auto& t : ts) o << " {maker " << t.makerOrderId() << " taker " << t.takerOrderId() << " @" << t.price() << " x" << t.quantity() << "}";
        return ts.empty() ? " none" : o.str();
    }

    void fail(const std::string& op, const std::string& what) {
        if (!failure_.empty()) return;
        std::ostringstream o;
        o << "[" << label_ << " seed=" << seed_ << " operation #" << ops_ << ": " << op << "] " << what;
        failure_ = o.str();
        std::cerr << "    MISMATCH " << failure_ << "\n";
    }

    lob::ExecutionReport finish(const std::string& op, const ref::Expected& want, const lob::ExecutionReport& got,
                                std::size_t modelOrdersBefore) {
        afterOperation(op, want, modelOrdersBefore, &got);
        return got;
    }

    // Common tail of every operation. `got` is null when the production operation was
    // performed externally (for example through the CLI) and has no ExecutionReport.
    void afterOperation(const std::string& op, const ref::Expected& want, std::size_t modelOrdersBefore,
                        const lob::ExecutionReport* got) {
        ++ops_;
        if (!failure_.empty()) return;
        // New ids the model recorded become tracked.
        if (model.orders().size() > modelOrdersBefore) ids.push_back(want.orderId);
        if (got != nullptr) compareReport(op, want, *got);
        if (!clean()) return;
        // Touched orders: the one named in the report and every counterparty.
        std::vector<lob::OrderId> touched;
        if (want.orderId != 0) touched.push_back(want.orderId);
        for (const auto& t : want.trades) touched.push_back(t.maker);
        for (lob::OrderId id : touched) compareOrder(op, id);
        compareTradeCounts(op);
        const bool periodic = ops_ % auditEvery_ == 0;
        // The whole-book comparison is O(orders); do it every operation only in the
        // fine-grained (audit-every-op) mode, otherwise on the audit schedule.
        if (clean() && (auditEvery_ == 1 || periodic)) compareBook(op);
        if (clean() && periodic) {
            fullCompare(op);
            fullAudit(op);
        }
    }

public:
    // For operations performed outside Duel (for example by the CLI on `manager`): the
    // caller applies the same operation to `model`, then reports it here so that the
    // touched orders, the book and the auditor are compared exactly as for a direct call.
    void observe(const std::string& op, const ref::Expected& want, std::size_t modelOrdersBefore) {
        afterOperation(op, want, modelOrdersBefore, nullptr);
    }

private:
    void compareReport(const std::string& op, const ref::Expected& want, const lob::ExecutionReport& got) {
        std::ostringstream why;
        auto note = [&why](const char* field, auto expected, auto actual) {
            why << " " << field << ": expected " << expected << " actual " << actual << ";";
        };
        bool bad = false;
        if (got.accepted != want.accepted) { note("accepted", want.accepted, got.accepted); bad = true; }
        if (got.rejectReason != want.reason) { note("rejectReason", lob::toString(want.reason), lob::toString(got.rejectReason)); bad = true; }
        if (got.orderId != want.orderId) { note("orderId", want.orderId, got.orderId); bad = true; }
        if (got.status != want.status) { note("status", lob::toString(want.status), lob::toString(got.status)); bad = true; }
        if (got.originalQuantity != want.original) { note("originalQuantity", want.original, got.originalQuantity); bad = true; }
        if (got.executedQuantity != want.executed) { note("executedQuantity", want.executed, got.executedQuantity); bad = true; }
        if (got.remainingQuantity != want.remaining) { note("remainingQuantity", want.remaining, got.remainingQuantity); bad = true; }
        if (got.cancelReason != want.cancelReason) { note("cancelReason", lob::toString(want.cancelReason), lob::toString(got.cancelReason)); bad = true; }
        bool tradesSame = got.trades.size() == want.trades.size();
        for (std::size_t i = 0; tradesSame && i < got.trades.size(); ++i) {
            const lob::Trade& g = got.trades[i];
            const ref::Trade& w = want.trades[i];
            tradesSame = g.makerOrderId() == w.maker && g.takerOrderId() == w.taker && g.price() == w.price &&
                         g.quantity() == w.quantity && g.takerSide() == w.takerSide && g.id() == w.id;
        }
        if (!tradesSame) {
            why << " trades: expected" << tradesText(want.trades) << " actual" << tradesText(got.trades) << ";";
            bad = true;
        }
        if (bad) fail(op, "the report differs from the reference model:" + why.str());
    }

    void compareOrder(const std::string& op, lob::OrderId id) {
        const auto it = model.orders().find(id);
        const std::optional<lob::OrderRecord> rec = manager.order(id);
        if (it == model.orders().end()) {
            if (rec) fail(op, "order " + std::to_string(id) + " exists in production but not in the model");
            return;
        }
        const ref::Order& o = it->second;
        if (!rec) {
            fail(op, "order " + std::to_string(id) + " is missing from production");
            return;
        }
        std::ostringstream why;
        bool bad = false;
        auto note = [&why, &bad](const char* field, auto expected, auto actual) {
            why << " " << field << ": expected " << expected << " actual " << actual << ";";
            bad = true;
        };
        if (rec->status != o.status) note("status", lob::toString(o.status), lob::toString(rec->status));
        if (rec->side != o.side) note("side", lob::toString(o.side), lob::toString(rec->side));
        if (rec->executedQuantity != o.executed) note("executed", o.executed, rec->executedQuantity);
        if (rec->remainingQuantity != o.remaining) note("remaining", o.remaining, rec->remainingQuantity);
        if (rec->cancelledQuantity != o.cancelled) note("cancelled", o.cancelled, rec->cancelledQuantity);
        if (rec->effectiveQuantity != o.effective) note("effective", o.effective, rec->effectiveQuantity);
        if (rec->submittedQuantity != o.submitted) note("submitted", o.submitted, rec->submittedQuantity);
        if (rec->price != o.price) note("price", o.price, rec->price);
        if (rec->originalPrice != o.originalPrice) note("originalPrice", o.originalPrice, rec->originalPrice);
        if (rec->sequence != o.stamp) note("sequence", o.stamp, rec->sequence);
        if (rec->submissionSequence != o.submissionStamp) note("submissionSequence", o.submissionStamp, rec->submissionSequence);
        if (rec->rejectReason != o.rejectReason) note("rejectReason", lob::toString(o.rejectReason), lob::toString(rec->rejectReason));
        if (rec->cancelReason != o.cancelReason) note("cancelReason", lob::toString(o.cancelReason), lob::toString(rec->cancelReason));
        std::vector<ref::Trade> wantTrades;
        for (std::size_t idx : o.tradeIndices) wantTrades.push_back(model.trades()[idx]);
        const std::vector<lob::Trade> gotTrades = manager.tradesFor(id);
        bool same = wantTrades.size() == gotTrades.size();
        for (std::size_t i = 0; same && i < gotTrades.size(); ++i) {
            same = gotTrades[i].id() == wantTrades[i].id && gotTrades[i].makerOrderId() == wantTrades[i].maker &&
                   gotTrades[i].takerOrderId() == wantTrades[i].taker && gotTrades[i].price() == wantTrades[i].price &&
                   gotTrades[i].quantity() == wantTrades[i].quantity;
        }
        if (!same) {
            why << " tradesFor: expected" << tradesText(wantTrades) << " actual" << tradesText(gotTrades) << ";";
            bad = true;
        }
        if (bad) fail(op, "order " + std::to_string(id) + " differs from the reference model:" + why.str());
    }

    void compareBook(const std::string& op) {
        const lob::OrderBook& book = manager.book();
        for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
            const std::vector<ref::Level> want = model.levels(side);
            const lob::MarketDepth depth = book.depth(std::numeric_limits<std::size_t>::max());
            const std::vector<lob::LevelSummary>& got = side == lob::Side::Buy ? depth.bids : depth.asks;
            bool same = want.size() == got.size();
            for (std::size_t i = 0; same && i < got.size(); ++i) {
                same = want[i].price == got[i].price && want[i].total == got[i].totalQuantity && want[i].count == got[i].orderCount;
            }
            if (!same) {
                std::ostringstream o;
                o << lob::toString(side) << " levels differ. expected:";
                for (const auto& l : want) o << " [" << l.price << " x" << l.total << " n" << l.count << "]";
                o << " actual:";
                for (const auto& l : got) o << " [" << l.price << " x" << l.totalQuantity << " n" << l.orderCount << "]";
                fail(op, o.str());
                return;
            }
        }
        const auto bid = model.bestPrice(lob::Side::Buy);
        const auto ask = model.bestPrice(lob::Side::Sell);
        if (bid.has_value() != book.bestBid().has_value() || (bid && *bid != book.bestBid()->price)) fail(op, "best bid differs");
        if (ask.has_value() != book.bestAsk().has_value() || (ask && *ask != book.bestAsk()->price)) fail(op, "best ask differs");
        const auto spread = model.spread();
        if (spread.has_value() != book.spread().has_value() || (spread && *spread != *book.spread())) fail(op, "spread differs");
        const auto mid = model.mid();
        const auto gotMid = book.midPrice();
        if (mid.has_value() != gotMid.has_value() ||
            (mid && (mid->floorTicks != static_cast<std::uint64_t>(gotMid->ticks) || mid->half != gotMid->halfTick))) {
            fail(op, "mid-price differs");
        }
        if (book.orderCount() != model.activeCount()) {
            fail(op, "active order count differs: expected " + std::to_string(model.activeCount()) + " actual " + std::to_string(book.orderCount()));
        }
    }

    void compareTradeCounts(const std::string& op) {
        if (manager.trades().size() != model.trades().size()) {
            fail(op, "global trade count: expected " + std::to_string(model.trades().size()) + " actual " + std::to_string(manager.trades().size()));
        }
        if (manager.recordedOrderCount() != model.orders().size()) {
            fail(op, "recorded order count: expected " + std::to_string(model.orders().size()) + " actual " + std::to_string(manager.recordedOrderCount()));
        }
    }

    void fullCompare(const std::string& op) {
        for (lob::OrderId id : ids) {
            compareOrder(op, id);
            if (!clean()) return;
        }
        compareBook(op);
        compareTradeCounts(op);
    }

    void fullAudit(const std::string& op) {
        const std::vector<std::string> v = auditor_.audit(manager, ids);
        if (!v.empty()) {
            std::string all;
            for (std::size_t i = 0; i < v.size() && i < 6; ++i) all += "\n      " + v[i];
            fail(op, "system invariant violation(s):" + all);
        }
    }

    std::string label_;
    std::uint64_t seed_;
    int auditEvery_;
    int ops_ = 0;
    std::string failure_;
    audit::Auditor auditor_;
};
