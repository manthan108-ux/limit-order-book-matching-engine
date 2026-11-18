// lob_alloc_profile: allocation behaviour of the benchmark scenarios, measured separately
// from timing (so the counting never perturbs a timed number).
//
// For every selected scenario it reports, for ONE run of the timed region:
//   allocations/op and bytes/op (requested bytes; allocator overhead is not included)
// and, for scenarios that build a resting state, the bytes retained by that state
// per resting order.
//
// Replaces global operator new/delete (counting, with a size header so unsized deletes can
// be accounted). This executable is linked statically on MinGW so that allocations made
// inside the shared libstdc++ also go through it.
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <string>
#include <vector>

#include "BenchHarness.h"
#include "Scenarios.h"

namespace counting {
bool enabled = false;
std::uint64_t allocations = 0;
std::uint64_t bytes = 0;
std::int64_t liveBytes = 0;  // always tracked (also when counting is off), to measure retained state
}  // namespace counting

namespace {
constexpr std::size_t kHeader = 16;  // keeps 16-byte alignment and stores the size
}

#if defined(_MSC_VER)
#define LOB_NOINLINE __declspec(noinline)
#else
#define LOB_NOINLINE __attribute__((noinline))
#endif

// Kept out of line so the compiler cannot reason about the size header across the pair.
LOB_NOINLINE void* operator new(std::size_t size) {
    void* raw = std::malloc(size + kHeader);
    if (raw == nullptr) throw std::bad_alloc();
    *static_cast<std::size_t*>(raw) = size;
    counting::liveBytes += static_cast<std::int64_t>(size);
    if (counting::enabled) {
        ++counting::allocations;
        counting::bytes += size;
    }
    return static_cast<char*>(raw) + kHeader;
}
LOB_NOINLINE void operator delete(void* p) noexcept {
    if (p == nullptr) return;
    void* raw = static_cast<char*>(p) - kHeader;
    counting::liveBytes -= static_cast<std::int64_t>(*static_cast<std::size_t*>(raw));
    std::free(raw);
}
LOB_NOINLINE void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

int main(int argc, char** argv) {
    bench::Sizes sizes;
    std::string group, filter, csvPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quick") sizes.divisor = 20.0;
        else if (a == "--group" && i + 1 < argc) group = argv[++i];
        else if (a == "--filter" && i + 1 < argc) filter = argv[++i];
        else if (a == "--csv" && i + 1 < argc) csvPath = argv[++i];
        else { std::cerr << "usage: lob_alloc_profile [--quick] [--group G] [--filter TEXT] [--csv FILE]\n"; return 2; }
    }
    if (!bench::kOptimizedBuild) {
        std::cerr << "error: build in Release (allocation counts of an unoptimized build differ)\n";
        return 2;
    }
    std::vector<bench::Scenario> scenarios = bench::buildScenarios(sizes);
    std::ofstream csv;
    if (!csvPath.empty()) {
        csv.open(csvPath);
        csv << "# allocation profile: one run of each scenario's timed region; bytes are REQUESTED bytes (no allocator overhead)\n";
        csv << "scenario,variant,params,group,ops,allocations,bytes,allocs_per_op,bytes_per_op,state_orders,state_retained_bytes,state_bytes_per_order\n";
    }
    std::cout << std::left << std::setw(72) << "scenario [variant] params" << std::right << std::setw(12) << "allocs/op" << std::setw(12) << "bytes/op"
              << std::setw(18) << "state bytes/order" << "\n";
    int failures = 0;
    for (bench::Scenario& s : scenarios) {
        if (!group.empty() && s.group != group) continue;
        if (!filter.empty() && (s.name + " " + s.variant + " " + s.params).find(filter) == std::string::npos) continue;
        // Memory retained by the freshly built state.
        const std::int64_t liveBefore = counting::liveBytes;
        s.setup();
        const std::int64_t retained = counting::liveBytes - liveBefore;
        if (s.validateSetup) {
            const std::string e = s.validateSetup();
            if (!e.empty()) { std::cout << "FAILED setup validation: " << e << "\n"; ++failures; continue; }
        }
        counting::allocations = 0;
        counting::bytes = 0;
        counting::enabled = true;
        s.timed();
        counting::enabled = false;
        const std::uint64_t allocs = counting::allocations, bytes = counting::bytes;
        if (s.validate) {
            const std::string e = s.validate();
            if (!e.empty()) { std::cout << "FAILED validation: " << e << "\n"; ++failures; continue; }
        }
        const double ops = static_cast<double>(s.opsPerRun);
        const double perOrder = s.stateOrders > 0 ? static_cast<double>(retained) / static_cast<double>(s.stateOrders) : 0.0;
        std::ostringstream label;
        label << s.name << " [" << s.variant << "] " << s.params;
        std::cout << std::left << std::setw(72) << label.str().substr(0, 71) << std::right << std::fixed << std::setprecision(2) << std::setw(12)
                  << static_cast<double>(allocs) / ops << std::setw(12) << static_cast<double>(bytes) / ops;
        if (s.stateOrders > 0) std::cout << std::setw(18) << std::setprecision(1) << perOrder;
        std::cout << "\n";
        if (csv) {
            csv << bench::csvEscape(s.name) << "," << bench::csvEscape(s.variant) << "," << bench::csvEscape(s.params) << "," << s.group << "," << s.opsPerRun
                << "," << allocs << "," << bytes << "," << std::fixed << std::setprecision(3) << static_cast<double>(allocs) / ops << ","
                << static_cast<double>(bytes) / ops << "," << s.stateOrders << "," << retained << "," << std::setprecision(1) << perOrder << "\n";
        }
    }
    return failures == 0 ? 0 : 1;
}
