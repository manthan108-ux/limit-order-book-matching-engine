// lob_bench: reproducible timing benchmarks for the matching engine.
//
//   lob_bench [--quick] [--reps N] [--warmup N] [--lat-reps N] [--no-latency]
//             [--group core|scale-active|scale-levels|scale-perlevel|scale-matching|scale-cancelmodify]
//             [--filter TEXT] [--csv FILE] [--pin CPU] [--list]
//
// Release builds only: an unoptimized build exits with an error rather than produce numbers
// that must never be compared with optimized ones. Correctness validation runs outside every
// timed region. See docs/BENCHMARKS.md for the methodology.
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "BenchHarness.h"
#include "Scenarios.h"

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void usage(std::ostream& os) {
    os << "usage: lob_bench [--quick] [--reps N] [--warmup N] [--lat-reps N] [--no-latency]\n"
          "                 [--group NAME] [--filter TEXT] [--csv FILE] [--pin CPU] [--list]\n"
          "  --quick       ~20x smaller workloads, 3 repetitions (smoke test; numbers are not for analysis)\n"
          "  --reps N      measured repetitions per scenario (default 21)\n"
          "  --warmup N    discarded warm-up runs (default 2)\n"
          "  --lat-reps N  latency-pass repetitions, pooled (default 3)\n"
          "  --group NAME  only scenarios of this group (core, scale-active, scale-levels, scale-perlevel,\n"
          "                scale-matching, scale-cancelmodify)\n"
          "  --filter TEXT only scenarios whose name/variant/params contain TEXT (case-insensitive)\n"
          "  --csv FILE    also write results as CSV (machine info in '#' header lines)\n"
          "  --pin CPU     pin the thread to one logical CPU (Windows)\n"
          "  --list        list the selected scenarios and exit\n";
}

}  // namespace

int main(int argc, char** argv) {
    bench::RunConfig cfg;
    cfg.reps = 21;
    bench::Sizes sizes;
    std::string csvPath, filter, group;
    bool list = false, quick = false;
    int pin = -1;
    std::string commandLine;
    for (int i = 1; i < argc; ++i) commandLine += (i > 1 ? " " : "") + std::string(argv[i]);

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "error: " << what << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--quick") quick = true;
        else if (a == "--reps") cfg.reps = std::atoi(next("--reps").c_str());
        else if (a == "--warmup") cfg.warmup = std::atoi(next("--warmup").c_str());
        else if (a == "--lat-reps") cfg.latencyReps = std::atoi(next("--lat-reps").c_str());
        else if (a == "--no-latency") cfg.latencyEnabled = false;
        else if (a == "--group") group = next("--group");
        else if (a == "--filter") filter = lower(next("--filter"));
        else if (a == "--csv") csvPath = next("--csv");
        else if (a == "--pin") pin = std::atoi(next("--pin").c_str());
        else if (a == "--list") list = true;
        else if (a == "--help" || a == "-h") { usage(std::cout); return 0; }
        else { std::cerr << "error: unknown option '" << a << "'\n"; usage(std::cerr); return 2; }
    }
    if (quick) {
        sizes.divisor = 20.0;
        cfg.reps = std::min(cfg.reps, 3);
        cfg.warmup = 1;
        cfg.latencyReps = 1;
    }
    if (cfg.reps < 1) cfg.reps = 1;

    if (!bench::kOptimizedBuild) {
        std::cerr << "error: lob_bench must be built in Release (NDEBUG); results from an unoptimized build are meaningless and must not be\n"
                     "compared with optimized ones. Configure with -DCMAKE_BUILD_TYPE=Release.\n";
        return 2;
    }

    std::vector<bench::Scenario> scenarios = bench::buildScenarios(sizes);
    std::vector<bench::Scenario*> selected;
    for (bench::Scenario& s : scenarios) {
        if (!group.empty() && s.group != group) continue;
        if (!filter.empty() && lower(s.name + " " + s.variant + " " + s.params + " " + s.group).find(filter) == std::string::npos) continue;
        selected.push_back(&s);
    }
    if (list) {
        for (const bench::Scenario* s : selected) std::cout << s->group << "\t" << s->name << "\t" << s->variant << "\t" << s->params << "\n";
        std::cout << selected.size() << " scenarios\n";
        return 0;
    }
    if (selected.empty()) {
        std::cerr << "no scenarios match\n";
        return 2;
    }

    const bool pinned = pin >= 0 && bench::pinToCpu(pin);
    if (pin >= 0 && !pinned) std::cerr << "warning: could not pin to CPU " << pin << "\n";
    const bench::ClockInfo clock = bench::measureClock();
    std::vector<bench::KeyValue> info = bench::machineInfo(clock, pinned ? pin : -1);
    info.push_back({"mode", quick ? "quick (smoke test, ~20x smaller workloads)" : "full"});
    info.push_back({"repetitions", std::to_string(cfg.reps)});
    info.push_back({"warmup_runs", std::to_string(cfg.warmup)});
    info.push_back({"latency_repetitions", std::to_string(cfg.latencyReps)});
    info.push_back({"latency_per_op_threshold_ns", std::to_string(static_cast<int>(bench::kPerOpMinNs))});

    std::cout << "lob_bench: " << selected.size() << " scenarios\n";
    for (const bench::KeyValue& kv : info) std::cout << "  " << kv.key << ": " << kv.value << "\n";
    std::cout << "  (latency 'op' = per-operation timing, includes ~" << static_cast<int>(clock.overheadNs) << " ns timer cost and is quantized to "
              << static_cast<int>(clock.resolutionNs) << " ns; 'bm' = percentiles of batch MEANS)\n\n";

    std::ofstream csv;
    if (!csvPath.empty()) {
        csv.open(csvPath);
        if (!csv) {
            std::cerr << "error: cannot write " << csvPath << "\n";
            return 2;
        }
        bench::writeCsvHeader(csv, info, commandLine);
    }

    int failures = 0;
    std::string lastGroup;
    for (bench::Scenario* s : selected) {
        if (s->group != lastGroup) {
            std::cout << "== " << s->group << " ==\n";
            lastGroup = s->group;
        }
        const bench::Result r = bench::runScenario(*s, cfg);
        bench::printRow(r);
        std::cout.flush();
        if (!r.error.empty()) {
            ++failures;
            continue;
        }
        if (r.medianRunMs < 1.0) std::cout << "    (warning: the timed region is under 1 ms; quantization may dominate)\n";
        if (csv) bench::writeCsvRows(csv, r);
    }
    std::cout << "\n" << (failures == 0 ? "all scenarios validated and measured" : std::to_string(failures) + " scenario(s) FAILED validation") << "\n";
    return failures == 0 ? 0 : 1;
}
