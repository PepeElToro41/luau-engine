#include "support/bench.hpp"

#include "engine/templates/sparse_list.hpp"

struct Element {
    u64 a = 0;
    u64 b = 0;
};

BENCH_CASE("templates/sparse_list: new + delete churn") {
    SparseList<Element> list;
    bench.run("new_element + delete_element", [&] {
        const SparseId id = list.new_element();
        list.delete_element(id);
    });
    list.free();
}

BENCH_CASE("templates/sparse_list: lookup") {
    constexpr usz count = 4096;
    SparseList<Element> list;
    DynamicArray<SparseId> ids;
    for (usz i = 0; i < count; ++i) {
        ids.push(list.new_element());
    }
    usz cursor = 0;
    bench.run("get_element_alive", [&] {
        cursor = (cursor + 1) % count;
        ankerl::nanobench::doNotOptimizeAway(list.get_element_alive(ids[cursor]));
    });
    bench.run("is_alive", [&] {
        cursor = (cursor + 1) % count;
        ankerl::nanobench::doNotOptimizeAway(list.is_alive(ids[cursor]));
    });
    ids.free();
    list.free();
}
