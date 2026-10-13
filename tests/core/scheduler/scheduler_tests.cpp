#include "support/test_support.hpp"

#include "engine/scheduler/scheduler.hpp"
#include "engine/utils/log.hpp"

#include <cstring>

// --- Helpers -------------------------------------------------------------------

// Every system appends its user_data (a tag character) to this log, so a
// test reads the order systems ran in as a string.
struct RunLog {
    char text[64] = {};
    usz count = 0;

    void push(const char tag) {
        if (this->count + 1 < sizeof(this->text)) {
            this->text[this->count++] = tag;
            this->text[this->count] = '\0';
        }
    }
    void clear() {
        this->count = 0;
        this->text[0] = '\0';
    }
};

// A system whose user_data points at a `Tagged`: records the tag.
struct Tagged {
    RunLog* log = nullptr;
    char tag = '?';
};

static void record_tag(SystemContext& ctx) {
    const Tagged* tagged = static_cast<const Tagged*>(ctx.user_data);
    tagged->log->push(tagged->tag);
}

// Collects the scheduler's log messages, for the tests that expect one.
// install() keeps stderr quiet too; remove() restores it.
struct MessageLog {
    u32 count = 0;
    char last[512] = {};
    LogSinkHandle handle;
    LogLevel stderr_level = LOG_INFO;

    void install() {
        this->stderr_level = LOG::stderr_level();
        LOG::set_stderr_level(LOG_LEVEL_NONE);
        this->handle = LOG::add_sink(&MessageLog::sink, this);
    }
    void remove() {
        LOG::remove_sink(this->handle);
        LOG::set_stderr_level(this->stderr_level);
    }

    static void sink(const LogRecord& record, void* user_data) {
        if (record.category != LOG_CATEGORY::SCHEDULER) {
            return;
        }
        MessageLog* log = static_cast<MessageLog*>(user_data);
        log->count += 1;
        snprintf(log->last, sizeof(log->last), "%s", record.text);
    }
};

// --- Pipelines, phases, systems ---------------------------------------------------

TEST_CASE("scheduler/scheduler: pipelines, phases and systems are added and found by name") {
    Scheduler scheduler;
    scheduler.init();

    const PipelineHandle update = scheduler.add_pipeline("update");
    const PipelineHandle render = scheduler.add_pipeline("render");
    REQUIRE(update.is_valid());
    REQUIRE(render.is_valid());
    CHECK(update != render);
    CHECK(scheduler.find_pipeline("update") == update);
    CHECK(scheduler.find_pipeline("render") == render);
    CHECK_FALSE(scheduler.find_pipeline("missing").is_valid());
    CHECK_FALSE(scheduler.find_pipeline(nullptr).is_valid());
    CHECK(strcmp(scheduler.pipeline(update)->name, "update") == 0);
    CHECK(scheduler.pipeline(update)->order == PIPELINE_ORDER_INSERTION);

    const PhaseHandle pre = scheduler.add_phase(update, "pre_update");
    const PhaseHandle main = scheduler.add_phase(update, "update");
    REQUIRE(pre.is_valid());
    REQUIRE(main.is_valid());
    CHECK(scheduler.find_phase(update, "pre_update") == pre);
    CHECK(scheduler.find_phase(update, "update") == main);
    CHECK(scheduler.phase(pre)->pipeline == update);

    SUBCASE("a phase name is scoped to its pipeline") {
        const PhaseHandle render_main = scheduler.add_phase(render, "update");
        REQUIRE(render_main.is_valid());
        CHECK(render_main != main);
        CHECK(scheduler.find_phase(render, "update") == render_main);
        CHECK_FALSE(scheduler.find_phase(render, "pre_update").is_valid());
    }
    SUBCASE("duplicate and bad names are refused") {
        MessageLog messages;
        messages.install();
        CHECK_FALSE(scheduler.add_pipeline("update").is_valid());
        CHECK_FALSE(scheduler.add_pipeline("").is_valid());
        CHECK_FALSE(scheduler.add_pipeline(nullptr).is_valid());
        CHECK_FALSE(scheduler.add_phase(update, "update").is_valid());
        CHECK_FALSE(scheduler.add_phase(update, "").is_valid());
        char long_name[SCHEDULER::NAME_MAX + 8];
        memset(long_name, 'x', sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = '\0';
        CHECK_FALSE(scheduler.add_phase(update, long_name).is_valid());
        CHECK(messages.count == 6);
        messages.remove();
    }
    SUBCASE("systems are found by name within their phase") {
        RunLog log;
        Tagged a{&log, 'a'};
        const SystemHandle system_a = scheduler.add_system(pre, "a", &record_tag, &a);
        REQUIRE(system_a.is_valid());
        CHECK(scheduler.find_system(pre, "a") == system_a);
        CHECK_FALSE(scheduler.find_system(main, "a").is_valid());
        CHECK(scheduler.system(system_a)->phase == pre);
        CHECK(scheduler.system(system_a)->pipeline == update);
        CHECK(scheduler.system(system_a)->user_data == &a);
        CHECK(scheduler.is_enabled(system_a));
        // The same name in another phase is another system.
        const SystemHandle main_a = scheduler.add_system(main, "a", &record_tag, &a);
        REQUIRE(main_a.is_valid());
        CHECK(main_a != system_a);
        CHECK_FALSE(scheduler.add_system(pre, "a", &record_tag, &a).is_valid());
        CHECK_FALSE(scheduler.add_system(pre, "b", nullptr, &a).is_valid());
    }
    SUBCASE("a stale pipeline or phase handle is refused everywhere") {
        PipelineHandle stale_pipeline;
        stale_pipeline.id = 12345;
        PhaseHandle stale_phase;
        stale_phase.id = 12345;
        CHECK_FALSE(scheduler.add_phase(stale_pipeline, "x").is_valid());
        CHECK_FALSE(scheduler.add_phase(PipelineHandle{}, "x").is_valid());
        CHECK_FALSE(scheduler.add_system(stale_phase, "x", &record_tag, nullptr).is_valid());
        CHECK_FALSE(scheduler.find_phase(stale_pipeline, "pre_update").is_valid());
        CHECK(scheduler.pipeline(stale_pipeline) == nullptr);
        CHECK(scheduler.phase(stale_phase) == nullptr);
        CHECK_FALSE(scheduler.remove_phase(stale_phase));
        CHECK_FALSE(scheduler.run(stale_pipeline, nullptr, nullptr, 0.0f));
    }

    scheduler.free();
    CHECK_ARENA_CLEAN();
}

// --- Order ----------------------------------------------------------------------

TEST_CASE("scheduler/scheduler: unconstrained phases and systems run in insertion order") {
    Scheduler scheduler;
    scheduler.init();
    RunLog log;
    Tagged a{&log, 'a'}, b{&log, 'b'}, c{&log, 'c'}, d{&log, 'd'};

    const PipelineHandle update = scheduler.add_pipeline("update");
    const PhaseHandle first = scheduler.add_phase(update, "first");
    const PhaseHandle second = scheduler.add_phase(update, "second");
    // Added out of phase order on purpose: the phase decides, then insertion.
    scheduler.add_system(second, "c", &record_tag, &c);
    scheduler.add_system(first, "a", &record_tag, &a);
    scheduler.add_system(second, "d", &record_tag, &d);
    scheduler.add_system(first, "b", &record_tag, &b);

    CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
    CHECK(strcmp(log.text, "abcd") == 0);

    SUBCASE("the order is stable across runs") {
        log.clear();
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "abcd") == 0);
    }
    SUBCASE("phase_order and system_order report it") {
        DynamicArray<PhaseHandle> phases;
        CHECK(scheduler.phase_order(update, phases));
        REQUIRE(phases.count == 2);
        CHECK(phases[0] == first);
        CHECK(phases[1] == second);
        DynamicArray<SystemHandle> systems;
        CHECK(scheduler.system_order(second, systems));
        REQUIRE(systems.count == 2);
        CHECK(systems[0] == scheduler.find_system(second, "c"));
        CHECK(systems[1] == scheduler.find_system(second, "d"));
        phases.free();
        systems.free();
    }
    SUBCASE("an empty phase keeps its place in phase_order") {
        const PhaseHandle middle = scheduler.add_phase(update, "middle");
        CHECK(scheduler.phase_after(middle, first));
        CHECK(scheduler.phase_before(middle, second));
        DynamicArray<PhaseHandle> phases;
        CHECK(scheduler.phase_order(update, phases));
        REQUIRE(phases.count == 3);
        CHECK(phases[1] == middle);
        phases.free();
        log.clear();
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "abcd") == 0);
    }

    scheduler.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scheduler/scheduler: a reverse-insertion pipeline runs the last added first") {
    Scheduler scheduler;
    scheduler.init();
    RunLog log;
    Tagged a{&log, 'a'}, b{&log, 'b'}, c{&log, 'c'}, d{&log, 'd'};

    const PipelineHandle shutdown = scheduler.add_pipeline("shutdown", PIPELINE_ORDER_REVERSE_INSERTION);
    const PhaseHandle pre = scheduler.add_phase(shutdown, "pre_shutdown");
    const PhaseHandle post = scheduler.add_phase(shutdown, "post_shutdown");
    CHECK(scheduler.phase_after(post, pre));
    // Two "plugins" adding in build order a, b: shutdown runs b before a
    // within each phase, and the phases keep their constraint.
    scheduler.add_system(pre, "a detach", &record_tag, &a);
    scheduler.add_system(post, "a free", &record_tag, &c);
    scheduler.add_system(pre, "b detach", &record_tag, &b);
    scheduler.add_system(post, "b free", &record_tag, &d);

    CHECK(scheduler.run(shutdown, nullptr, nullptr, 0.0f));
    CHECK(strcmp(log.text, "badc") == 0);

    SUBCASE("unconstrained phases reverse too") {
        const PhaseHandle extra = scheduler.add_phase(shutdown, "extra");
        Tagged e{&log, 'e'};
        scheduler.add_system(extra, "e", &record_tag, &e);
        log.clear();
        CHECK(scheduler.run(shutdown, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "ebadc") == 0);
    }

    scheduler.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scheduler/scheduler: after and before constraints order phases and systems") {
    Scheduler scheduler;
    scheduler.init();
    RunLog log;
    Tagged a{&log, 'a'}, b{&log, 'b'}, c{&log, 'c'}, d{&log, 'd'};

    const PipelineHandle update = scheduler.add_pipeline("update");
    const PhaseHandle late = scheduler.add_phase(update, "late");
    const PhaseHandle early = scheduler.add_phase(update, "early");
    CHECK(scheduler.phase_before(early, late));
    const SystemHandle system_a = scheduler.add_system(late, "a", &record_tag, &a);
    const SystemHandle system_b = scheduler.add_system(late, "b", &record_tag, &b);
    const SystemHandle system_c = scheduler.add_system(late, "c", &record_tag, &c);
    scheduler.add_system(early, "d", &record_tag, &d);

    SUBCASE("system_after moves a system behind another, the rest keep insertion order") {
        CHECK(scheduler.system_after(system_a, system_c));
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "dbca") == 0);
    }
    SUBCASE("system_before is the same constraint the other way") {
        CHECK(scheduler.system_before(system_c, system_a));
        CHECK(scheduler.system_edges.count == 1);
        CHECK(scheduler.system_edges[0].system == system_a);
        CHECK(scheduler.system_edges[0].after == system_c);
        // b is unconstrained and earlier than c, so it still goes first.
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "dbca") == 0);
    }
    SUBCASE("a constraint by name, without holding the other system") {
        CHECK(scheduler.system_after(scheduler.find_system(late, "b"), scheduler.find_system(late, "c")));
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "dacb") == 0);
    }
    SUBCASE("a chain") {
        CHECK(scheduler.system_after(system_a, system_b));
        CHECK(scheduler.system_after(system_b, system_c));
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "dcba") == 0);
    }
    SUBCASE("the same constraint twice is kept once") {
        CHECK(scheduler.system_after(system_a, system_b));
        CHECK(scheduler.system_after(system_a, system_b));
        CHECK(scheduler.system_edges.count == 1);
    }
    SUBCASE("a constraint against itself, across phases or across pipelines is refused") {
        MessageLog messages;
        messages.install();
        CHECK_FALSE(scheduler.system_after(system_a, system_a));
        CHECK_FALSE(scheduler.system_after(system_a, scheduler.find_system(early, "d")));
        CHECK_FALSE(scheduler.phase_after(late, late));
        const PipelineHandle render = scheduler.add_pipeline("render");
        const PhaseHandle draw = scheduler.add_phase(render, "draw");
        CHECK_FALSE(scheduler.phase_after(draw, early));
        CHECK_FALSE(scheduler.phase_after(early, draw));
        CHECK(messages.count == 5);
        CHECK(scheduler.system_edges.count == 0);
        CHECK(scheduler.phase_edges.count == 1);
        // A stale handle fails silently: it is a valid state, not a mistake.
        CHECK(scheduler.remove_system(system_c));
        CHECK_FALSE(scheduler.system_after(system_a, system_c));
        CHECK(messages.count == 5);
        messages.remove();
    }

    scheduler.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scheduler/scheduler: a cycle breaks only its pipeline until an edit removes it") {
    Scheduler scheduler;
    scheduler.init();
    RunLog log;
    Tagged a{&log, 'a'}, b{&log, 'b'}, c{&log, 'c'}, r{&log, 'r'};
    MessageLog messages;
    messages.install();

    const PipelineHandle update = scheduler.add_pipeline("update");
    const PhaseHandle main = scheduler.add_phase(update, "update");
    const SystemHandle system_a = scheduler.add_system(main, "a", &record_tag, &a);
    const SystemHandle system_b = scheduler.add_system(main, "b", &record_tag, &b);
    const SystemHandle system_c = scheduler.add_system(main, "c", &record_tag, &c);
    const PipelineHandle render = scheduler.add_pipeline("render");
    const PhaseHandle draw = scheduler.add_phase(render, "draw");
    scheduler.add_system(draw, "r", &record_tag, &r);

    SUBCASE("systems in a cycle") {
        CHECK(scheduler.system_after(system_a, system_b));
        CHECK(scheduler.system_after(system_b, system_c));
        CHECK(scheduler.system_after(system_c, system_a));
        CHECK_FALSE(scheduler.compile());
        CHECK(messages.count == 1);
        CHECK(strstr(messages.last, "phase 'update'") != nullptr);
        CHECK(strstr(messages.last, "a, b, c") != nullptr);
        CHECK(scheduler.pipeline(update)->broken);
        CHECK_FALSE(scheduler.pipeline(render)->broken);

        // Nothing of the broken pipeline runs, the other one does, and the
        // cycle is not reported again on every run.
        CHECK_FALSE(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(log.count == 0);
        CHECK(scheduler.run(render, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "r") == 0);
        CHECK_FALSE(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(messages.count == 1);
        DynamicArray<SystemHandle> systems;
        CHECK_FALSE(scheduler.system_order(main, systems));
        CHECK(systems.count == 0);
        systems.free();

        // Removing a system of the cycle (and its edges) repairs it.
        CHECK(scheduler.remove_system(system_c));
        CHECK(scheduler.compile());
        CHECK_FALSE(scheduler.pipeline(update)->broken);
        log.clear();
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "ba") == 0);
    }
    SUBCASE("phases in a cycle") {
        const PhaseHandle other = scheduler.add_phase(update, "other");
        CHECK(scheduler.phase_after(main, other));
        CHECK(scheduler.phase_after(other, main));
        CHECK_FALSE(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(messages.count == 1);
        CHECK(strstr(messages.last, "pipeline 'update'") != nullptr);
        CHECK(strstr(messages.last, "update, other") != nullptr);
        DynamicArray<PhaseHandle> phases;
        CHECK_FALSE(scheduler.phase_order(update, phases));
        CHECK(phases.count == 0);
        phases.free();
        // Removing the empty phase takes its constraints with it.
        CHECK(scheduler.remove_phase(other));
        CHECK(scheduler.phase_edges.count == 0);
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "abc") == 0);
    }
    messages.remove();

    scheduler.free();
    CHECK_ARENA_CLEAN();
}

// --- Running --------------------------------------------------------------------

struct ContextProbe {
    SystemContext seen;
    u32 calls = 0;
};

static void probe_context(SystemContext& ctx) {
    ContextProbe* probe = static_cast<ContextProbe*>(ctx.user_data);
    probe->seen = ctx;
    probe->calls += 1;
}

TEST_CASE("scheduler/scheduler: a system receives the run's host, argument and dt plus its own data and handles") {
    Scheduler scheduler;
    scheduler.init();
    ContextProbe probe;
    int host = 0;
    int argument = 0;

    const PipelineHandle render = scheduler.add_pipeline("render");
    const PhaseHandle draw = scheduler.add_phase(render, "draw");
    const SystemHandle system = scheduler.add_system(draw, "probe", &probe_context, &probe);

    CHECK(scheduler.run(render, &host, &argument, 0.25f));
    CHECK(probe.calls == 1);
    CHECK(probe.seen.scheduler == &scheduler);
    CHECK(probe.seen.host == &host);
    CHECK(probe.seen.argument == &argument);
    CHECK(probe.seen.dt == 0.25f);
    CHECK(probe.seen.user_data == &probe);
    CHECK(probe.seen.system == system);
    CHECK(probe.seen.phase == draw);
    CHECK(probe.seen.pipeline == render);

    scheduler.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scheduler/scheduler: a disabled system is skipped and keeps its place") {
    Scheduler scheduler;
    scheduler.init();
    RunLog log;
    Tagged a{&log, 'a'}, b{&log, 'b'}, c{&log, 'c'};

    const PipelineHandle update = scheduler.add_pipeline("update");
    const PhaseHandle main = scheduler.add_phase(update, "update");
    scheduler.add_system(main, "a", &record_tag, &a);
    const SystemHandle system_b = scheduler.add_system(main, "b", &record_tag, &b);
    scheduler.add_system(main, "c", &record_tag, &c);

    CHECK(scheduler.set_enabled(system_b, false));
    CHECK_FALSE(scheduler.is_enabled(system_b));
    // Not a structural change: the order was compiled by the first run and
    // stays compiled.
    CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
    CHECK(strcmp(log.text, "ac") == 0);
    CHECK_FALSE(scheduler.dirty);
    CHECK(scheduler.set_enabled(system_b, true));
    CHECK_FALSE(scheduler.dirty);
    log.clear();
    CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
    CHECK(strcmp(log.text, "abc") == 0);

    DynamicArray<SystemHandle> systems;
    CHECK(scheduler.set_enabled(system_b, false));
    CHECK(scheduler.system_order(main, systems));
    CHECK(systems.count == 3);
    systems.free();
    CHECK_FALSE(scheduler.set_enabled(SystemHandle{}, true));
    CHECK_FALSE(scheduler.is_enabled(SystemHandle{}));

    scheduler.free();
    CHECK_ARENA_CLEAN();
}

// A system that edits the scheduler while it runs.
struct Editing {
    RunLog* log = nullptr;
    char tag = '?';
    Scheduler* scheduler = nullptr;
    SystemHandle remove;
    PhaseHandle add_to;
    Tagged* add = nullptr;
    PipelineHandle run_nested;
};

static void edit_while_running(SystemContext& ctx) {
    Editing* editing = static_cast<Editing*>(ctx.user_data);
    editing->log->push(editing->tag);
    if (editing->remove.is_valid()) {
        CHECK(editing->scheduler->remove_system(editing->remove));
        editing->remove = SystemHandle{};
    }
    if (editing->add_to.is_valid()) {
        CHECK(editing->scheduler->add_system(editing->add_to, "added", &record_tag, editing->add).is_valid());
        editing->add_to = PhaseHandle{};
    }
    if (editing->run_nested.is_valid()) {
        CHECK(editing->scheduler->run(editing->run_nested, nullptr, nullptr, 0.0f));
        editing->run_nested = PipelineHandle{};
    }
}

TEST_CASE("scheduler/scheduler: edits during a run take effect without disturbing it") {
    Scheduler scheduler;
    scheduler.init();
    RunLog log;
    Tagged a{&log, 'a'}, c{&log, 'c'}, d{&log, 'd'}, n{&log, 'n'};
    Editing b{&log, 'b', &scheduler};

    const PipelineHandle update = scheduler.add_pipeline("update");
    const PhaseHandle main = scheduler.add_phase(update, "update");
    scheduler.add_system(main, "a", &record_tag, &a);
    scheduler.add_system(main, "b", &edit_while_running, &b);
    const SystemHandle system_c = scheduler.add_system(main, "c", &record_tag, &c);
    scheduler.add_system(main, "d", &record_tag, &d);

    SUBCASE("a system removed by an earlier one is skipped this run") {
        b.remove = system_c;
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "abd") == 0);
        CHECK(scheduler.system(system_c) == nullptr);
        log.clear();
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "abd") == 0);
    }
    SUBCASE("a system added during a run first runs on the next one") {
        b.add_to = main;
        b.add = &n;
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "abcd") == 0);
        log.clear();
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "abcdn") == 0);
    }
    SUBCASE("a system may run another pipeline, which rebuilds the shared order") {
        const PipelineHandle render = scheduler.add_pipeline("render");
        const PhaseHandle draw = scheduler.add_phase(render, "draw");
        Tagged r{&log, 'r'};
        scheduler.add_system(draw, "r", &record_tag, &r);
        b.run_nested = render;
        b.add_to = main;   // dirty before the nested run compiles
        b.add = &n;
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "abrcd") == 0);
        log.clear();
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        CHECK(strcmp(log.text, "abcdn") == 0);
    }

    scheduler.free();
    CHECK_ARENA_CLEAN();
}

// --- Removal and generations --------------------------------------------------------

TEST_CASE("scheduler/scheduler: a removed handle stops matching and is never aliased by a new one") {
    Scheduler scheduler;
    scheduler.init();
    RunLog log;
    Tagged a{&log, 'a'}, b{&log, 'b'};

    const PipelineHandle update = scheduler.add_pipeline("update");
    const PhaseHandle main = scheduler.add_phase(update, "update");
    const SystemHandle system_a = scheduler.add_system(main, "a", &record_tag, &a);
    const SystemHandle system_b = scheduler.add_system(main, "b", &record_tag, &b);
    CHECK(scheduler.system_after(system_b, system_a));

    CHECK(scheduler.remove_system(system_a));
    CHECK(scheduler.system(system_a) == nullptr);
    CHECK_FALSE(scheduler.remove_system(system_a));
    CHECK_FALSE(scheduler.set_enabled(system_a, false));
    CHECK_FALSE(scheduler.find_system(main, "a").is_valid());
    CHECK(scheduler.system_edges.count == 0);

    // The slot is reused with a new generation: the old handle still fails.
    const SystemHandle system_c = scheduler.add_system(main, "c", &record_tag, &a);
    REQUIRE(system_c.is_valid());
    CHECK(system_c != system_a);
    CHECK(scheduler.system(system_a) == nullptr);
    CHECK(scheduler.system(system_c) != nullptr);
    CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
    CHECK(strcmp(log.text, "ba") == 0);

    SUBCASE("a phase with systems is not removed; an emptied one is") {
        MessageLog messages;
        messages.install();
        CHECK_FALSE(scheduler.remove_phase(main));
        CHECK(messages.count == 1);
        CHECK(scheduler.phase(main) != nullptr);
        CHECK(scheduler.remove_system(system_b));
        CHECK(scheduler.remove_system(system_c));
        CHECK(scheduler.remove_phase(main));
        CHECK(scheduler.phase(main) == nullptr);
        CHECK_FALSE(scheduler.find_phase(update, "update").is_valid());
        CHECK_FALSE(scheduler.add_system(main, "x", &record_tag, &a).is_valid());
        const PhaseHandle again = scheduler.add_phase(update, "update");
        CHECK(again.is_valid());
        CHECK(again != main);
        CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));
        messages.remove();
    }

    scheduler.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scheduler/scheduler: free returns the scheduler to its initial state") {
    Scheduler scheduler;
    scheduler.init();
    const PipelineHandle update = scheduler.add_pipeline("update");
    const PhaseHandle main = scheduler.add_phase(update, "update");
    RunLog log;
    Tagged a{&log, 'a'};
    scheduler.add_system(main, "a", &record_tag, &a);
    CHECK(scheduler.run(update, nullptr, nullptr, 0.0f));

    scheduler.free();
    CHECK(scheduler.pipelines.alive_count == 0);
    CHECK(scheduler.order.count == 0);
    CHECK(scheduler.dirty);
    CHECK_FALSE(scheduler.find_pipeline("update").is_valid());
    CHECK_ARENA_CLEAN();

    // Usable again after init.
    scheduler.init();
    CHECK(scheduler.add_pipeline("update").is_valid());
    scheduler.free();
    CHECK_ARENA_CLEAN();
}
