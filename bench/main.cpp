#define ANKERL_NANOBENCH_IMPLEMENT
#include "support/bench.hpp"

#include "engine/defines.hpp"

#include <cstdio>
#include <cstring>

// Usage:
//   engine_bench                 run every benchmark
//   engine_bench ecs/component   run benchmarks whose name contains any argument
//   engine_bench --list          print benchmark names and exit

static bool name_matches(const char* name, const int argc, char** argv) {
    if (argc <= 1) {
        return true;
    }
    for (int i = 1; i < argc; ++i) {
        if (strstr(name, argv[i]) != nullptr) {
            return true;
        }
    }
    return false;
}

int main(const int argc, char** argv) {
    DynamicArray<BenchEntry>& entries = BENCH::entries();

    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        for (const BenchEntry& entry : entries) {
            printf("%s\n", entry.name);
        }
        return 0;
    }

#if ENGINE_DEBUG
    fprintf(stderr, "warning: Debug build (ENGINE_ASSERT on, no optimization); numbers are not representative.\n");
#endif

    usz ran = 0;
    for (const BenchEntry& entry : entries) {
        if (!name_matches(entry.name, argc, argv)) {
            continue;
        }
        ankerl::nanobench::Bench bench;
        bench.title(entry.name).unit("op").warmup(100).minEpochIterations(1000).performanceCounters(true).relative(false);
        entry.fn(bench);
        ran += 1;
    }

    if (ran == 0) {
        fprintf(stderr, "no benchmark matched; use --list to see names\n");
        return 1;
    }
    return 0;
}
