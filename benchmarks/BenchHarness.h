#pragma once
// Measurement infrastructure for lob_bench (no external benchmarking dependency).
//
// Methodology (see docs/BENCHMARKS.md for the full description):
//  * Clock: std::chrono::steady_clock. Its resolution and per-call cost are MEASURED at
//    start-up and printed, because they bound what can honestly be reported
//    (on the reference machine: 100 ns tick, ~40 ns per call).
//  * Throughput: one timed region per repetition around a whole batch of operations (no
//    per-operation timing inside it), so timer cost is amortized to nothing. Reported
//    as ns/operation (median / min / max over repetitions) and operations/second.
//  * Latency: a SEPARATE pass, never mixed into the throughput numbers.
//      - per-op: every operation timed individually. Used only when an operation takes at
//        least kPerOpMinNs, so the 100 ns quantization and ~40 ns timer cost are small
//        relative to it. These samples INCLUDE the timer overhead (not subtracted).
//      - batch-mean: for faster operations, batches of B operations are timed and each
//        sample is (batch time / B). The percentiles are then percentiles of batch MEANS,
//        which understate the true per-operation tail; they are labelled as such.
//  * Every repetition rebuilds its state (untimed), unless the scenario is read-only.
//    Correctness validation runs outside every timed region, after setup and after the run.
//  * Warm-up runs are executed and discarded before measuring.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace bench {

using Clock = std::chrono::steady_clock;

inline std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// Keeps the compiler from deleting a computation whose result is otherwise unused.
template <class T>
inline void doNotOptimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&value) : "memory");
#else
    static volatile const void* sink;
    sink = &value;
#endif
}
inline void clobberMemory() {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : : "memory");
#endif
}

// Per-operation timing is only trusted for operations at least this slow.
constexpr double kPerOpMinNs = 2000.0;
// A batch-mean sample should span at least this much time (>= 200 clock ticks).
constexpr double kBatchTargetNs = 20000.0;

// ---- Statistics ------------------------------------------------------------------------------

struct Summary {
    double median = 0, min = 0, max = 0, mean = 0, stddev = 0, cvPercent = 0;
    std::size_t n = 0;
};

inline Summary summarize(std::vector<double> v) {
    Summary s;
    s.n = v.size();
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    s.median = v.size() % 2 == 1 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    double sq = 0;
    for (double x : v) sq += (x - s.mean) * (x - s.mean);
    s.stddev = v.size() > 1 ? std::sqrt(sq / static_cast<double>(v.size() - 1)) : 0.0;
    s.cvPercent = s.mean > 0 ? 100.0 * s.stddev / s.mean : 0.0;
    return s;
}

// Nearest-rank percentile of an already SORTED vector (p in [0, 100]).
inline double percentileSorted(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    const double rank = std::ceil(p / 100.0 * static_cast<double>(sorted.size()));
    const std::size_t idx = rank < 1 ? 0 : static_cast<std::size_t>(rank) - 1;
    return sorted[std::min(idx, sorted.size() - 1)];
}

// ---- Machine and clock information -------------------------------------------------------------

struct ClockInfo {
    double resolutionNs = 0;  // smallest non-zero step observed
    double overheadNs = 0;    // cost of one now() call
};

inline ClockInfo measureClock() {
    ClockInfo c;
    std::int64_t smallest = INT64_MAX;
    for (int i = 0; i < 100000; ++i) {
        const std::int64_t a = nowNs();
        std::int64_t b = nowNs();
        while (b == a) b = nowNs();
        smallest = std::min(smallest, b - a);
    }
    c.resolutionNs = static_cast<double>(smallest);
    const int n = 1000000;
    std::int64_t sink = 0;
    const std::int64_t t0 = nowNs();
    for (int i = 0; i < n; ++i) sink += nowNs() & 1;
    const std::int64_t t1 = nowNs();
    doNotOptimize(sink);
    c.overheadNs = static_cast<double>(t1 - t0) / n;
    return c;
}

inline std::string cpuBrand() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int regs[12] = {0};
    if (__get_cpuid(0x80000000u, &regs[0], &regs[1], &regs[2], &regs[3]) && regs[0] >= 0x80000004u) {
        __get_cpuid(0x80000002u, &regs[0], &regs[1], &regs[2], &regs[3]);
        __get_cpuid(0x80000003u, &regs[4], &regs[5], &regs[6], &regs[7]);
        __get_cpuid(0x80000004u, &regs[8], &regs[9], &regs[10], &regs[11]);
        char text[49] = {0};
        for (int i = 0; i < 12; ++i) {
            for (int k = 0; k < 4; ++k) text[i * 4 + k] = static_cast<char>((regs[i] >> (8 * k)) & 0xFF);
        }
        std::string s(text);
        const std::size_t first = s.find_first_not_of(' ');
        return first == std::string::npos ? s : s.substr(first);
    }
#endif
    return "unknown";
}

inline double totalRamGb() {
#if defined(_WIN32)
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) return static_cast<double>(m.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
#endif
    return 0;
}

// Pins the calling thread to one logical CPU (Windows only). Returns true on success.
inline bool pinToCpu(int cpu) {
#if defined(_WIN32)
    if (cpu < 0 || cpu >= 64) return false;
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << cpu) != 0;
#else
    (void)cpu;
    return false;
#endif
}

#ifndef LOB_BENCH_COMPILER
#define LOB_BENCH_COMPILER "unknown"
#endif
#ifndef LOB_BENCH_BUILD_TYPE
#define LOB_BENCH_BUILD_TYPE "unknown"
#endif
#ifndef LOB_BENCH_FLAGS
#define LOB_BENCH_FLAGS "unknown"
#endif

constexpr bool kOptimizedBuild =
#if defined(NDEBUG)
    true;
#else
    false;
#endif

struct KeyValue {
    std::string key, value;
};

inline std::vector<KeyValue> machineInfo(const ClockInfo& clock, int pinnedCpu) {
    std::vector<KeyValue> v;
    v.push_back({"cpu", cpuBrand()});
    v.push_back({"logical_cpus", std::to_string(std::thread::hardware_concurrency())});
    std::ostringstream ram;
    ram << std::fixed << std::setprecision(1) << totalRamGb();
    v.push_back({"ram_gb", ram.str()});
#if defined(_WIN32)
    v.push_back({"os", "Windows"});
#elif defined(__linux__)
    v.push_back({"os", "Linux"});
#else
    v.push_back({"os", "other"});
#endif
    v.push_back({"compiler", LOB_BENCH_COMPILER});
    v.push_back({"build_type", LOB_BENCH_BUILD_TYPE});
    v.push_back({"cxx_flags", LOB_BENCH_FLAGS});
    v.push_back({"ndebug", kOptimizedBuild ? "yes" : "no"});
    std::ostringstream res;
    res << std::fixed << std::setprecision(1) << clock.resolutionNs << " ns";
    v.push_back({"clock_resolution", res.str()});
    std::ostringstream ov;
    ov << std::fixed << std::setprecision(1) << clock.overheadNs << " ns";
    v.push_back({"clock_call_overhead", ov.str()});
    v.push_back({"pinned_cpu", pinnedCpu >= 0 ? std::to_string(pinnedCpu) : "none"});
    return v;
}

// ---- Scenarios ----------------------------------------------------------------------------------------

struct LatencyGroup {
    std::string name;            // "" = the whole scenario; otherwise e.g. an operation kind
    std::string method;          // "per_op" or "batch_mean(B)"
    std::vector<double> samplesNs;
};

struct Scenario {
    std::string name;        // family, e.g. "submit_resting"
    std::string variant;     // e.g. "layer=manager"
    std::string params;      // e.g. "M=100000;L=1000"
    std::string group;       // "core" or "scale-*" (for --filter)
    std::size_t opsPerRun = 0;
    bool readOnly = false;   // setup once; the timed region never changes the state
    int maxReps = 1 << 30;   // cap for very large states (their setup dominates the wall time)
    bool perOpOnly = false;  // latency must be per-op (e.g. grouped by operation kind)
    std::size_t stateOrders = 0;  // resting orders the setup builds (0 = not meaningful); for bytes/order

    std::function<void()> setup;                  // fresh state (untimed)
    std::function<void()> timed;                  // the timed region: performs opsPerRun operations
    std::function<std::string()> validateSetup;   // after setup, before timing; "" = ok
    std::function<std::string()> validate;        // after each run; "" = ok
    // Latency pass: batch <= 1 means per-op. May append several groups.
    std::function<void(std::size_t batch, std::vector<LatencyGroup>&)> latency;
    // Extra facts for the report, evaluated after a run (e.g. "trades=12345").
    std::function<std::string()> note;
};

// Binds a scenario's operation. `op(i)` performs operation number i; the same callable is
// used for the throughput loop and the latency loops, so both measure identical work.
template <class Op>
void bindOps(Scenario& s, std::size_t ops, Op op) {
    s.opsPerRun = ops;
    s.timed = [ops, op]() mutable {
        for (std::size_t i = 0; i < ops; ++i) op(i);
    };
    s.latency = [ops, op](std::size_t batch, std::vector<LatencyGroup>& out) mutable {
        LatencyGroup g;
        if (batch <= 1) {
            g.method = "per_op";
            g.samplesNs.reserve(ops);
            for (std::size_t i = 0; i < ops; ++i) {
                const std::int64_t t0 = nowNs();
                op(i);
                const std::int64_t t1 = nowNs();
                g.samplesNs.push_back(static_cast<double>(t1 - t0));
            }
        } else {
            g.method = "batch_mean(" + std::to_string(batch) + ")";
            g.samplesNs.reserve(ops / batch + 1);
            for (std::size_t i = 0; i < ops; i += batch) {
                const std::size_t n = std::min(batch, ops - i);
                const std::int64_t t0 = nowNs();
                for (std::size_t k = 0; k < n; ++k) op(i + k);
                const std::int64_t t1 = nowNs();
                g.samplesNs.push_back(static_cast<double>(t1 - t0) / static_cast<double>(n));
            }
        }
        out.push_back(std::move(g));
    };
}

// ---- Results ------------------------------------------------------------------------------------------------

struct LatencyResult {
    std::string group, method;
    double p50 = 0, p95 = 0, p99 = 0, max = 0;
    std::size_t samples = 0;
};

struct Result {
    Scenario* scenario = nullptr;
    int warmup = 0, reps = 0;
    Summary nsPerOp;                 // over repetitions
    double throughputMedian = 0;     // operations per second, from the median repetition
    double throughputBest = 0;       // from the fastest repetition
    double medianRunMs = 0;          // wall time of the timed region of the median repetition
    std::vector<LatencyResult> latency;
    std::string note;
    std::string error;               // non-empty = validation failed
};

struct RunConfig {
    int warmup = 2;
    int reps = 21;
    int latencyReps = 3;
    bool latencyEnabled = true;
};

inline Result runScenario(Scenario& s, const RunConfig& cfg) {
    Result r;
    r.scenario = &s;
    r.warmup = cfg.warmup;
    const int reps = std::min(cfg.reps, s.maxReps);
    r.reps = reps;
    auto fail = [&r](const std::string& where, const std::string& what) {
        r.error = where + ": " + what;
        return r;
    };

    if (s.readOnly) {
        s.setup();
        if (s.validateSetup) {
            const std::string e = s.validateSetup();
            if (!e.empty()) return fail("setup validation", e);
        }
    }
    const int warm = std::min(cfg.warmup, s.maxReps < cfg.warmup ? 1 : cfg.warmup);
    for (int w = 0; w < warm; ++w) {  // discarded
        if (!s.readOnly) s.setup();
        s.timed();
        clobberMemory();
    }

    std::vector<double> nsPerOp;
    std::vector<double> runMs;
    for (int i = 0; i < reps; ++i) {
        if (!s.readOnly) {
            s.setup();
            if (s.validateSetup) {
                const std::string e = s.validateSetup();
                if (!e.empty()) return fail("setup validation", e);
            }
        }
        clobberMemory();
        const std::int64_t t0 = nowNs();
        s.timed();
        const std::int64_t t1 = nowNs();
        clobberMemory();
        if (s.validate) {
            const std::string e = s.validate();
            if (!e.empty()) return fail("result validation (repetition " + std::to_string(i) + ")", e);
        }
        nsPerOp.push_back(static_cast<double>(t1 - t0) / static_cast<double>(s.opsPerRun));
        runMs.push_back(static_cast<double>(t1 - t0) / 1e6);
    }
    r.nsPerOp = summarize(nsPerOp);
    r.throughputMedian = r.nsPerOp.median > 0 ? 1e9 / r.nsPerOp.median : 0;
    r.throughputBest = r.nsPerOp.min > 0 ? 1e9 / r.nsPerOp.min : 0;
    r.medianRunMs = summarize(runMs).median;
    if (s.note) r.note = s.note();

    if (cfg.latencyEnabled && s.latency && cfg.latencyReps > 0) {
        const bool perOp = s.perOpOnly || r.nsPerOp.median >= kPerOpMinNs;
        std::size_t batch = 0;
        if (!perOp) {
            batch = static_cast<std::size_t>(std::ceil(kBatchTargetNs / std::max(1.0, r.nsPerOp.median)));
            batch = std::max<std::size_t>(batch, 16);
        }
        std::vector<LatencyGroup> pooled;
        for (int i = 0; i < std::min(cfg.latencyReps, std::max(1, reps)); ++i) {
            if (!s.readOnly) s.setup();
            std::vector<LatencyGroup> one;
            s.latency(batch, one);
            if (s.validate) {
                const std::string e = s.validate();
                if (!e.empty()) return fail("latency-pass validation", e);
            }
            for (LatencyGroup& g : one) {
                auto it = std::find_if(pooled.begin(), pooled.end(), [&g](const LatencyGroup& p) { return p.name == g.name; });
                if (it == pooled.end()) {
                    pooled.push_back(std::move(g));
                } else {
                    it->samplesNs.insert(it->samplesNs.end(), g.samplesNs.begin(), g.samplesNs.end());
                }
            }
        }
        for (LatencyGroup& g : pooled) {
            std::sort(g.samplesNs.begin(), g.samplesNs.end());
            LatencyResult lr;
            lr.group = g.name;
            lr.method = g.method;
            lr.samples = g.samplesNs.size();
            lr.p50 = percentileSorted(g.samplesNs, 50);
            lr.p95 = percentileSorted(g.samplesNs, 95);
            lr.p99 = percentileSorted(g.samplesNs, 99);
            lr.max = g.samplesNs.empty() ? 0 : g.samplesNs.back();
            r.latency.push_back(lr);
        }
    }
    return r;
}

// ---- Output --------------------------------------------------------------------------------------------------------

inline std::string csvEscape(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

inline void writeCsvHeader(std::ostream& os, const std::vector<KeyValue>& info, const std::string& commandLine) {
    for (const KeyValue& kv : info) os << "# " << kv.key << ": " << kv.value << "\n";
    os << "# command_line: " << commandLine << "\n";
    os << "scenario,variant,params,group,ops_per_run,reps,warmup,median_ns_per_op,min_ns_per_op,max_ns_per_op,mean_ns_per_op,"
          "stddev_ns,cv_percent,throughput_median_ops_per_s,throughput_best_ops_per_s,median_run_ms,"
          "lat_group,lat_method,lat_samples,lat_p50_ns,lat_p95_ns,lat_p99_ns,lat_max_ns,note\n";
}

inline void writeCsvRows(std::ostream& os, const Result& r) {
    const Scenario& s = *r.scenario;
    auto common = [&](std::ostream& o) {
        o << csvEscape(s.name) << "," << csvEscape(s.variant) << "," << csvEscape(s.params) << "," << csvEscape(s.group) << ","
          << s.opsPerRun << "," << r.reps << "," << r.warmup << "," << std::fixed << std::setprecision(2) << r.nsPerOp.median << ","
          << r.nsPerOp.min << "," << r.nsPerOp.max << "," << r.nsPerOp.mean << "," << r.nsPerOp.stddev << "," << r.nsPerOp.cvPercent << ","
          << std::setprecision(0) << r.throughputMedian << "," << r.throughputBest << "," << std::setprecision(3) << r.medianRunMs << ",";
    };
    if (r.latency.empty()) {
        common(os);
        os << ",,,,,,," << csvEscape(r.note) << "\n";
        return;
    }
    for (const LatencyResult& l : r.latency) {
        common(os);
        os << csvEscape(l.group) << "," << csvEscape(l.method) << "," << l.samples << "," << std::fixed << std::setprecision(1)
           << l.p50 << "," << l.p95 << "," << l.p99 << "," << l.max << "," << csvEscape(r.note) << "\n";
    }
}

inline std::string fmtNs(double v) {
    std::ostringstream o;
    if (v >= 1e6) o << std::fixed << std::setprecision(2) << v / 1e6 << "ms";
    else if (v >= 1e3) o << std::fixed << std::setprecision(2) << v / 1e3 << "us";
    else o << std::fixed << std::setprecision(1) << v << "ns";
    return o.str();
}

inline void printRow(const Result& r) {
    const Scenario& s = *r.scenario;
    std::ostringstream label;
    label << s.name << " [" << s.variant << "] " << s.params;
    std::cout << std::left << std::setw(70) << label.str().substr(0, 69) << std::right << " ";
    if (!r.error.empty()) {
        std::cout << "FAILED: " << r.error << "\n";
        return;
    }
    std::cout << std::setw(10) << fmtNs(r.nsPerOp.median) << "/op  min " << std::setw(9) << fmtNs(r.nsPerOp.min) << "  max " << std::setw(9)
              << fmtNs(r.nsPerOp.max) << "  cv " << std::fixed << std::setprecision(1) << std::setw(5) << r.nsPerOp.cvPercent << "%  "
              << std::setw(8) << std::setprecision(2) << r.throughputMedian / 1e6 << " Mop/s";
    for (const LatencyResult& l : r.latency) {
        std::cout << "  | " << (l.group.empty() ? "" : l.group + " ") << (l.method == "per_op" ? "op" : "bm") << " p50 " << fmtNs(l.p50) << " p95 "
                  << fmtNs(l.p95) << " p99 " << fmtNs(l.p99);
    }
    if (!r.note.empty()) std::cout << "  [" << r.note << "]";
    std::cout << "\n";
}

}  // namespace bench
