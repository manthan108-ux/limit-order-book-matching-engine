// Deterministic randomized workloads: production vs the independent reference model
// (and the system auditor) after every operation. Every workload is fully determined
// by (seed, profile, size): a failure prints exactly that, plus the failing operation
// and the expected-vs-actual state, so it can be replayed by running that one workload.
//
// Seeds and sizes (all fixed; none depends on time, addresses or the platform):
//   small   60 ops    x 12 seeds x 3 profiles   audit + full comparison after EVERY op
//   medium  500 ops   x  6 seeds x 3 profiles   audit + full comparison after EVERY op
//   large   6000 ops  x  2 seeds x 2 profiles   touched orders every op; book + audit every 200
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "Cli.h"
#include "Duel.h"
#include "TestFramework.h"
#include "TestRng.h"

using namespace lob;

namespace {

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

struct Profile {
    const char* name;
    std::int64_t mid;       // price the market starts around
    std::int64_t spread;    // half-width of the price band around the drifting mid
    std::int64_t maxQty;
};

// Narrow: constant crossing, deep queues at few prices. Wide: many price levels.
// Mixed: moderate band, larger quantities (partial fills of big orders).
const Profile kNarrow{"narrow", 1000, 3, 20};
const Profile kWide{"wide", 1000, 60, 40};
const Profile kMixed{"mixed", 500, 12, 300};

class Workload {
public:
    Workload(const Profile& p, std::uint64_t seed, int ops, int auditEvery)
        : profile_(p), seed_(seed), ops_(ops),
          duel_(std::string(p.name) + "/" + std::to_string(ops) + "ops", seed, auditEvery), rng_(seed), mid_(p.mid) {}

    Duel& duel() { return duel_; }

    void run() {
        for (int i = 0; i < ops_ && duel_.clean(); ++i) {
            if (i % 40 == 0) mid_ += rng_.between(-2, 2);  // the market drifts
            step();
        }
        duel_.checkEverything("end of workload");
    }

private:
    Price priceFor(Side side) {
        // Buyers lean below the mid, sellers above, so the book builds depth but crosses often.
        const std::int64_t lean = side == B ? -1 : 1;
        Price p = mid_ + lean + rng_.between(-profile_.spread, profile_.spread);
        if (rng_.chance(3)) p += lean * rng_.between(0, profile_.spread * 3);  // an occasional far order
        return p < 1 ? 1 : p;
    }
    Quantity qty() { return rng_.between(1, profile_.maxQty); }
    Side side() { return rng_.chance(50) ? B : S; }

    // Target for a cancel / modify. Mostly a live order (otherwise nearly every cancel
    // would hit an already-terminal one and the interesting transitions would be rare),
    // sometimes any recorded id (terminal orders), sometimes an unknown id.
    OrderId pickKnown() {
        if (duel_.ids.empty() || rng_.chance(5)) return 9000000 + rng_.below(1000);  // unknown id
        for (int attempts = 0; attempts < 4 && !live_.empty() && rng_.chance(80); ++attempts) {
            const std::size_t i = rng_.below(live_.size());
            const auto it = duel_.model.orders().find(live_[i]);
            if (it != duel_.model.orders().end() && it->second.active()) return live_[i];
            live_[i] = live_.back();  // it went terminal: drop it from the pool
            live_.pop_back();
        }
        return duel_.ids[rng_.below(duel_.ids.size())];
    }

    // Remembers an accepted order that may still be resting.
    void remember(const ExecutionReport& r) {
        if (r.accepted && r.remainingQuantity > 0) live_.push_back(r.orderId);
    }

    void step() {
        const std::uint64_t r = rng_.below(100);
        if (r < 30) {                                   // submit, generated id
            const Side s = side();
            remember(duel_.submit(s, priceFor(s), qty()));
        } else if (r < 40) {                            // submit, client id
            const Side s = side();
            remember(duel_.submit(1000000 + nextClient_++, s, priceFor(s), qty()));
        } else if (r < 43) {                            // duplicate id (active or terminal)
            if (duel_.ids.empty()) return;
            const Side s = side();
            duel_.submit(duel_.ids[rng_.below(duel_.ids.size())], s, priceFor(s), qty());
        } else if (r < 48) {                            // invalid submissions
            const Side s = side();
            switch (rng_.below(5)) {
                case 0: duel_.submit(s, 0, qty()); break;
                case 1: duel_.submit(s, -rng_.between(1, 50), qty()); break;
                case 2: duel_.submit(s, priceFor(s), 0); break;
                case 3: duel_.submit(s, priceFor(s), -rng_.between(1, 50)); break;
                default: duel_.submit(0, s, priceFor(s), qty()); break;   // id 0
            }
        } else if (r < 63) {                            // cancel (mostly live, some terminal, some unknown)
            duel_.cancel(pickKnown());
        } else if (r < 85) {                            // modify
            std::optional<Price> p;
            std::optional<Quantity> q;
            if (rng_.chance(55)) p = rng_.chance(4) ? 0 : mid_ + rng_.between(-profile_.spread, profile_.spread);
            if (rng_.chance(55)) q = rng_.chance(4) ? -rng_.between(0, 5) : qty();
            remember(duel_.modify(pickKnown(), p, q));
        } else if (r < 93) {                            // a cluster of orders at one price (deep FIFO queue)
            const Side s = side();
            const Price p = priceFor(s);
            const int n = static_cast<int>(rng_.between(3, 6));
            for (int i = 0; i < n && duel_.clean(); ++i) remember(duel_.submit(s, p, qty()));
        } else {                                        // an aggressive sweep across levels
            const Side s = side();
            const Price p = s == B ? mid_ + profile_.spread * 2 : std::max<Price>(1, mid_ - profile_.spread * 2);
            remember(duel_.submit(s, p, profile_.maxQty * rng_.between(2, 8)));
        }
    }

    const Profile& profile_;
    std::uint64_t seed_;
    int ops_;
    Duel duel_;
    TestRng rng_;
    Price mid_;
    OrderId nextClient_ = 1;
    std::vector<OrderId> live_;
};

std::uint64_t checksum(const OrderManager& m) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    for (const Trade& t : m.trades()) {
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

void runAll(const Profile& p, const std::vector<std::uint64_t>& seeds, int ops, int auditEvery) {
    for (const std::uint64_t seed : seeds) {
        Workload w(p, seed, ops, auditEvery);
        w.run();
        CHECK_MSG(w.duel().clean(), w.duel().failure());
        if (!w.duel().clean()) return;  // one reproducible failure is enough
    }
}

const std::vector<std::uint64_t> kSmallSeeds = {1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233};
const std::vector<std::uint64_t> kMediumSeeds = {7, 42, 2024, 99991, 314159, 271828};
const std::vector<std::uint64_t> kLargeSeeds = {20260101, 8675309};

}  // namespace

TEST(SmallWorkloadsNarrowBand) { runAll(kNarrow, kSmallSeeds, 60, 1); }
TEST(SmallWorkloadsWideBand) { runAll(kWide, kSmallSeeds, 60, 1); }
TEST(SmallWorkloadsMixedQuantities) { runAll(kMixed, kSmallSeeds, 60, 1); }

TEST(MediumWorkloadsNarrowBand) { runAll(kNarrow, kMediumSeeds, 500, 1); }
TEST(MediumWorkloadsWideBand) { runAll(kWide, kMediumSeeds, 500, 1); }
TEST(MediumWorkloadsMixedQuantities) { runAll(kMixed, kMediumSeeds, 500, 1); }

TEST(LargeWorkloadsNarrowBand) { runAll(kNarrow, kLargeSeeds, 6000, 200); }
TEST(LargeWorkloadsWideBand) { runAll(kWide, kLargeSeeds, 6000, 200); }

TEST(WorkloadsActuallyExerciseTheInterestingStates) {
    // A workload that never trades, cancels or modifies would prove nothing.
    Workload w(kNarrow, 42, 1000, 1);
    w.run();
    CHECK_MSG(w.duel().clean(), w.duel().failure());
    const OrderManager& m = w.duel().manager;
    CHECK(m.trades().size() > 100);
    std::size_t filled = 0, partial = 0, cancelled = 0, rejected = 0, active = 0, multiTrade = 0, modified = 0;
    for (OrderId id : w.duel().ids) {
        const auto rec = m.order(id);
        if (!rec) continue;
        filled += rec->status == OrderStatus::Filled;
        partial += rec->status == OrderStatus::PartiallyFilled;
        cancelled += rec->status == OrderStatus::Cancelled;
        rejected += rec->status == OrderStatus::Rejected;
        active += rec->isActive();
        multiTrade += rec->tradeIndices.size() >= 3;
        modified += rec->effectiveQuantity != rec->submittedQuantity || rec->price != rec->originalPrice;
    }
    CHECK(filled > 20);
    CHECK(partial > 0);
    CHECK(cancelled > 20);
    CHECK(rejected > 10);
    CHECK(active > 5);
    CHECK(multiTrade > 5);
    CHECK(modified > 20);
}

TEST(WorkloadsAreReproducibleFromTheirSeed) {
    Workload a(kMixed, 123456, 800, 50);
    Workload b(kMixed, 123456, 800, 50);
    Workload c(kMixed, 123457, 800, 50);
    a.run();
    b.run();
    c.run();
    CHECK(a.duel().clean() && b.duel().clean() && c.duel().clean());
    CHECK_EQ(checksum(a.duel().manager), checksum(b.duel().manager));  // same seed: identical history
    CHECK(checksum(a.duel().manager) != checksum(c.duel().manager));   // different seed: different history
}

// ===== The CLI driven by random text, compared with the model ===============================

namespace {

std::string spaced(TestRng& rng, const std::string& word) {
    // Random case, so that "SuBmIt" and friends are exercised.
    std::string out = word;
    for (char& c : out) {
        if (rng.chance(30) && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

}  // namespace

TEST(CliRandomCommandsAgreeWithTheModelAndNeverMisreportSuccess) {
    for (const std::uint64_t seed : {11u, 222u, 3333u}) {
        Duel duel("cli-random", seed, 1);
        std::ostringstream out, err;
        Cli cli(duel.manager, out, err, Cli::Options{false});
        TestRng rng(seed);
        std::vector<OrderId> known;
        OrderId nextClient = 1;
        std::size_t malformed = 0, executed = 0;

        for (int i = 0; i < 1500 && duel.clean(); ++i) {
            std::string line;
            const std::uint64_t kind = rng.below(100);
            const std::string sideWord = rng.chance(50) ? "buy" : "sell";
            const std::int64_t price = 95 + rng.between(0, 10);
            const std::int64_t quantity = rng.between(1, 30);
            OrderId target = known.empty() || rng.chance(8) ? 777777 : known[rng.below(known.size())];
            for (int attempts = 0; attempts < 6 && !known.empty() && target != 777777 && rng.chance(85); ++attempts) {
                const auto it = duel.model.orders().find(target);  // prefer a live order
                if (it != duel.model.orders().end() && it->second.active()) break;
                target = known[rng.below(known.size())];
            }
            bool malformedLine = false;
            if (kind < 35) {
                line = spaced(rng, "submit") + " " + sideWord + " " + std::to_string(price) + " " + std::to_string(quantity);
                if (rng.chance(30)) line += " " + std::to_string(500000 + nextClient++);
            } else if (kind < 50) {
                line = spaced(rng, "cancel") + " " + std::to_string(target);
            } else if (kind < 70) {
                line = spaced(rng, "modify") + " " + std::to_string(target) + " " + std::to_string(price) + " " +
                       std::to_string(rng.between(1, 30));
            } else if (kind < 78) {
                static const char* const queries[] = {"book", "best", "depth 3", "help"};
                line = queries[rng.below(4)];
            } else if (kind < 82) {
                line = rng.chance(50) ? "order " + std::to_string(target) : "trades " + std::to_string(target);
            } else {  // malformed on purpose
                static const char* const bad[] = {"submit buy 10.5 5", "submit hold 1 1", "submit buy -3 5", "submit buy 5 0",
                                                  "cancel", "cancel -1", "modify 1 0 5", "modify 1 5 0", "depth 0",
                                                  "frobnicate", "submit buy 1e3 5", "book extra", "cancel 1 2",
                                                  "submit buy 99999999999999999999 1"};
                line = bad[rng.below(14)];
                malformedLine = true;
            }
            if (rng.chance(25)) line = "  \t" + line + "   ";  // stray whitespace

            const std::size_t recordsBefore = duel.manager.recordedOrderCount();
            const std::size_t tradesBefore = duel.manager.trades().size();
            out.str("");
            err.str("");

            // Mirror the command into the model exactly when the parser accepts it.
            const ParseResult parsed = parseCommand(line);
            const std::size_t modelBefore = duel.model.orders().size();
            ref::Expected want;
            bool mutating = false;
            if (parsed.ok()) {
                const ParsedCommand& c = parsed.command;
                if (c.type == CommandType::Submit) {
                    mutating = true;
                    want = c.orderId ? duel.model.submit(*c.orderId, c.side, c.price, c.quantity)
                                     : duel.model.submitGenerated(c.side, c.price, c.quantity);
                } else if (c.type == CommandType::Cancel) {
                    mutating = true;
                    want = duel.model.cancel(*c.orderId);
                } else if (c.type == CommandType::Modify) {
                    mutating = true;
                    want = duel.model.modify(*c.orderId, c.price, c.quantity);
                }
            }
            cli.handleLine(line);
            ++executed;

            if (!parsed.ok()) {
                ++malformed;
                CHECK_MSG(!err.str().empty() && out.str().empty(), "malformed line [" + line + "] produced no error or produced output");
                CHECK_MSG(duel.manager.recordedOrderCount() == recordsBefore && duel.manager.trades().size() == tradesBefore,
                          "malformed line [" + line + "] changed the system");
                CHECK(malformedLine || parsed.command.type == CommandType::Empty);
                continue;
            }
            CHECK_MSG(err.str().empty(), "a valid line [" + line + "] wrote to the error stream: " + err.str());
            if (mutating) {
                const std::string text = out.str();
                const std::string first = text.substr(0, text.find('\n'));  // the verdict is on the first line
                const ParsedCommand& c = parsed.command;
                const bool said = c.type == CommandType::Submit   ? first.find(": ACCEPTED") != std::string::npos
                                  : c.type == CommandType::Cancel ? first.find(": CANCELLED") != std::string::npos
                                                                  : first.find(": MODIFIED") != std::string::npos;
                const bool saidRejected = first.find("REJECTED") != std::string::npos;
                CHECK_MSG(said != saidRejected, "verdict line is neither success nor rejection for [" + line + "]: " + first);
                // The CLI may only say "success" if the operation really succeeded.
                CHECK_MSG(said == want.accepted, "CLI success message disagrees with the outcome for [" + line + "]: " + text);
                if (c.type == CommandType::Submit && want.accepted) known.push_back(want.orderId);
                duel.observe("cli: " + line, want, modelBefore);
            } else {
                CHECK(duel.manager.recordedOrderCount() == recordsBefore && duel.manager.trades().size() == tradesBefore);
            }
        }
        duel.checkEverything("end of CLI workload");
        CHECK_MSG(duel.clean(), duel.failure());
        CHECK(malformed > 100);   // the malformed branch really ran
        CHECK(executed > 1000);
    }
}

TEST_MAIN()
