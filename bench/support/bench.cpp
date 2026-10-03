#include "support/bench.hpp"

namespace BENCH {

DynamicArray<BenchEntry>& entries() {
    // Function-local so it is constructed before any BenchRegistrar runs,
    // whatever the static-initialization order across translation units.
    static DynamicArray<BenchEntry> list;
    return list;
}

void register_bench(const char* name, const BenchFn fn) {
    entries().push({name, fn});
}

} // namespace BENCH
