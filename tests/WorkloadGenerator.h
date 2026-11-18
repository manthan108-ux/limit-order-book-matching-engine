#pragma once
// The seeded workload generator, shared by the randomized correctness tests
// (RandomizedWorkloadTests.cpp, where the target is a Duel) and the benchmarks
// (benchmarks/, where the target records and replays the operations), so that the
// workloads that are checked for correctness and the workloads that are timed come from
// ONE generator and are comparable.
//
// A Target provides (all operations return the production ExecutionReport):
//   ExecutionReport submit(Side, Price, Quantity);                 // generated id
//   ExecutionReport submit(OrderId, Side, Price, Quantity);        // client id
//   ExecutionReport cancel(OrderId);
//   ExecutionReport modify(OrderId, std::optional<Price>, std::optional<Quantity>);
//   const std::vector<OrderId>& knownIds() const;                  // every id ever recorded, in order
//   bool isActive(OrderId) const;                                  // currently resting?
//   bool healthy() const;                                          // false stops the workload
//
// The sequence of random numbers consumed per step is part of the contract: changing it
// changes every workload for a given seed. tests/RandomizedWorkloadTests.cpp pins this with
// digests of known seeds.
#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "ExecutionReport.h"
#include "OrderTypes.h"
#include "TestRng.h"

namespace workload {

struct Profile {
    const char* name;
    std::int64_t mid;      // price the market starts around
    std::int64_t spread;   // half-width of the price band around the drifting mid
    std::int64_t maxQty;
};

// Narrow: constant crossing, deep queues at few prices. Wide: many price levels.
// Mixed: moderate band, larger quantities (partial fills of big orders).
inline const Profile kNarrow{"narrow", 1000, 3, 20};
inline const Profile kWide{"wide", 1000, 60, 40};
inline const Profile kMixed{"mixed", 500, 12, 300};

template <typename Target>
class Generator {
public:
    Generator(Target& target, const Profile& profile, std::uint64_t seed)
        : target_(target), profile_(profile), rng_(seed), mid_(profile.mid) {}

    // Runs `ops` steps (stopping early if the target stops being healthy). A step is one
    // operation, or a burst of 3-6 same-price submissions.
    void run(int ops) {
        for (int i = 0; i < ops && target_.healthy(); ++i) {
            if (i % 40 == 0) mid_ += rng_.between(-2, 2);  // the market drifts
            step();
        }
    }

private:
    using Side = lob::Side;
    using Price = lob::Price;
    using Quantity = lob::Quantity;
    using OrderId = lob::OrderId;

    static constexpr Side B = Side::Buy;
    static constexpr Side S = Side::Sell;

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
        const std::vector<OrderId>& ids = target_.knownIds();
        if (ids.empty() || rng_.chance(5)) return 9000000 + rng_.below(1000);  // unknown id
        for (int attempts = 0; attempts < 4 && !live_.empty() && rng_.chance(80); ++attempts) {
            const std::size_t i = rng_.below(live_.size());
            if (target_.isActive(live_[i])) return live_[i];
            live_[i] = live_.back();  // it went terminal: drop it from the pool
            live_.pop_back();
        }
        return ids[rng_.below(ids.size())];
    }

    // Remembers an accepted order that may still be resting.
    void remember(const lob::ExecutionReport& r) {
        if (r.accepted && r.remainingQuantity > 0) live_.push_back(r.orderId);
    }

    void step() {
        const std::uint64_t r = rng_.below(100);
        if (r < 30) {                                   // submit, generated id
            const Side s = side();
            remember(target_.submit(s, priceFor(s), qty()));
        } else if (r < 40) {                            // submit, client id
            const Side s = side();
            remember(target_.submit(1000000 + nextClient_++, s, priceFor(s), qty()));
        } else if (r < 43) {                            // duplicate id (active or terminal)
            const std::vector<OrderId>& ids = target_.knownIds();
            if (ids.empty()) return;
            const Side s = side();
            target_.submit(ids[rng_.below(ids.size())], s, priceFor(s), qty());
        } else if (r < 48) {                            // invalid submissions
            const Side s = side();
            switch (rng_.below(5)) {
                case 0: target_.submit(s, 0, qty()); break;
                case 1: target_.submit(s, -rng_.between(1, 50), qty()); break;
                case 2: target_.submit(s, priceFor(s), 0); break;
                case 3: target_.submit(s, priceFor(s), -rng_.between(1, 50)); break;
                default: target_.submit(0, s, priceFor(s), qty()); break;   // id 0
            }
        } else if (r < 63) {                            // cancel (mostly live, some terminal, some unknown)
            target_.cancel(pickKnown());
        } else if (r < 85) {                            // modify
            std::optional<Price> p;
            std::optional<Quantity> q;
            if (rng_.chance(55)) p = rng_.chance(4) ? 0 : mid_ + rng_.between(-profile_.spread, profile_.spread);
            if (rng_.chance(55)) q = rng_.chance(4) ? -rng_.between(0, 5) : qty();
            remember(target_.modify(pickKnown(), p, q));
        } else if (r < 93) {                            // a cluster of orders at one price (deep FIFO queue)
            const Side s = side();
            const Price p = priceFor(s);
            const int n = static_cast<int>(rng_.between(3, 6));
            for (int i = 0; i < n && target_.healthy(); ++i) remember(target_.submit(s, p, qty()));
        } else {                                        // an aggressive sweep across levels
            const Side s = side();
            const Price p = s == B ? mid_ + profile_.spread * 2 : std::max<Price>(1, mid_ - profile_.spread * 2);
            remember(target_.submit(s, p, profile_.maxQty * rng_.between(2, 8)));
        }
    }

    Target& target_;
    const Profile& profile_;
    TestRng rng_;
    Price mid_;
    OrderId nextClient_ = 1;
    std::vector<OrderId> live_;
};

}  // namespace workload
