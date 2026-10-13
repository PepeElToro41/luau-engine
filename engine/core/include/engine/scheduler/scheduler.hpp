#pragma once

#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/templates/sparse_list.hpp"

// The scheduler: what runs when. A pipeline is an entry point its owner
// invokes at a fixed moment (the Engine runs "update" once per frame,
// "render" with the frame, "startup" and "shutdown" once); it is a list of
// phases, each a list of systems. Phases are ordered against other phases
// of the same pipeline and systems against other systems of the same phase
// with after / before constraints; what is left unconstrained keeps the
// order it was added in (or the reverse, for a pipeline created with
// PIPELINE_ORDER_REVERSE_INSERTION: the shutdown pipeline, where the last
// thing built is the first torn down).
//
//     Scheduler scheduler;
//     scheduler.init();
//     PipelineHandle update = scheduler.add_pipeline("update");
//     PhaseHandle pre = scheduler.add_phase(update, "pre_update");
//     PhaseHandle main = scheduler.add_phase(update, "update");
//     scheduler.phase_after(main, pre);                       // pre, then main
//     SystemHandle input = scheduler.add_system(pre, "input", &poll_input, &state);
//     SystemHandle move = scheduler.add_system(main, "move", &move_things, &state);
//     ...
//     scheduler.run(update, &engine, nullptr, dt);            // every enabled system, in order
//     scheduler.free();
//
// A system is a function pointer and a user pointer; it receives a
// SystemContext with the run's `host` (the Engine, which core does not
// name) and `argument` (whatever the entry point was invoked with: the
// render pipeline's FrameContext), the delta time and its own data.
// Single-threaded: systems run one after the other on the calling thread.
// The dependency graph is what a parallel runner would consume later.
//
// Handles carry a generation (SparseList ids): one taken on a phase or
// system that was since removed stops matching, so every call with it
// fails and returns false / an invalid handle. Names are unique per scope
// (pipelines in the scheduler, phases in a pipeline, systems in a phase)
// and are how one plugin orders itself against another's step without
// holding that plugin: find_system(phase, "render").
//
// Every structural edit sets `dirty`; the next run (or order query)
// rebuilds the order of every pipeline. A cycle among the phases of a
// pipeline, or among the systems of one of its phases, is reported once as
// a LOG_CATEGORY::SCHEDULER error and marks that pipeline `broken`: run() returns false for
// it, and nothing of it runs, until an edit removes the cycle. Other
// pipelines are unaffected.
//
// Edits during a run are allowed: run() walks a copy of the order taken
// when it started, checks each system is still alive and enabled right
// before calling it, and anything added runs from the next run on. A
// system may run another pipeline (the engine's render from inside
// update), which rebuilds the shared order cache while the outer run keeps
// its copy.

namespace SCHEDULER {

// Capacity of a name, terminator included.
constexpr u32 NAME_MAX = 64;
constexpr u64 INVALID_HANDLE = 0xffffffffffffffffull;

} // namespace SCHEDULER

// --- Handles -------------------------------------------------------------------

struct PipelineHandle {
    u64 id = SCHEDULER::INVALID_HANDLE;

    bool is_valid() const { return this->id != SCHEDULER::INVALID_HANDLE; }
    bool operator==(const PipelineHandle& other) const { return this->id == other.id; }
    bool operator!=(const PipelineHandle& other) const { return this->id != other.id; }
};

struct PhaseHandle {
    u64 id = SCHEDULER::INVALID_HANDLE;

    bool is_valid() const { return this->id != SCHEDULER::INVALID_HANDLE; }
    bool operator==(const PhaseHandle& other) const { return this->id == other.id; }
    bool operator!=(const PhaseHandle& other) const { return this->id != other.id; }
};

struct SystemHandle {
    u64 id = SCHEDULER::INVALID_HANDLE;

    bool is_valid() const { return this->id != SCHEDULER::INVALID_HANDLE; }
    bool operator==(const SystemHandle& other) const { return this->id == other.id; }
    bool operator!=(const SystemHandle& other) const { return this->id != other.id; }
};

// --- Systems -------------------------------------------------------------------

struct Scheduler;

// What a system receives. `host` and `argument` are the run's, verbatim;
// `user_data` is the system's own; the handles say which system is running,
// in case it wants to remove or disable itself.
struct SystemContext {
    Scheduler* scheduler = nullptr;
    void* host = nullptr;
    void* argument = nullptr;
    f32 dt = 0.0f;
    void* user_data = nullptr;
    SystemHandle system;
    PhaseHandle phase;
    PipelineHandle pipeline;
};

using SystemFn = void (*)(SystemContext& ctx);

// --- Records -------------------------------------------------------------------

enum PipelineOrder : u32 {
    // Unconstrained phases and systems run in the order they were added.
    PIPELINE_ORDER_INSERTION = 0,
    // The last added runs first: for teardown.
    PIPELINE_ORDER_REVERSE_INSERTION = 1,
};

struct PipelineRecord {
    char name[SCHEDULER::NAME_MAX];
    u64 name_hash;
    PipelineOrder order;
    // Position in insertion order, scheduler-wide.
    u64 sequence;
    // The pipeline's slice of Scheduler::order once compiled.
    usz order_first;
    usz order_count;
    // A cycle was found in its phases or in one of its phases' systems.
    bool broken;
};

struct PhaseRecord {
    char name[SCHEDULER::NAME_MAX];
    u64 name_hash;
    PipelineHandle pipeline;
    u64 sequence;
};

struct SystemRecord {
    char name[SCHEDULER::NAME_MAX];
    u64 name_hash;
    PhaseHandle phase;
    PipelineHandle pipeline;
    SystemFn fn;
    void* user_data;
    u64 sequence;
    bool enabled;
};

// One entry of the compiled order: a system to call, with its phase.
struct ScheduleEntry {
    SystemHandle system;
    PhaseHandle phase;
};

// --- Scheduler ------------------------------------------------------------------

struct Scheduler {
    void init(BaseAllocator* allocator = nullptr);
    void free();

    // --- Pipelines ------------------------------------------------------------

    // Invalid (with a message) if the name is empty, too long or taken.
    PipelineHandle add_pipeline(const char* name, PipelineOrder order = PIPELINE_ORDER_INSERTION);
    PipelineHandle find_pipeline(const char* name) const;
    // The record, or nullptr for a stale handle.
    PipelineRecord* pipeline(PipelineHandle handle);
    const PipelineRecord* pipeline(PipelineHandle handle) const;

    // --- Phases ---------------------------------------------------------------

    // A phase of `pipeline`. Invalid (with a message) if the pipeline is
    // stale or the name is empty, too long or taken in that pipeline.
    PhaseHandle add_phase(PipelineHandle pipeline, const char* name);
    PhaseHandle find_phase(PipelineHandle pipeline, const char* name) const;
    // Removes an empty phase and every constraint naming it. False (with a
    // message) when systems remain in it, or for a stale handle.
    bool remove_phase(PhaseHandle handle);
    PhaseRecord* phase(PhaseHandle handle);
    const PhaseRecord* phase(PhaseHandle handle) const;
    // `phase` runs after / before `other`. False for a stale handle, a
    // phase against itself or one of another pipeline; a constraint given
    // twice is kept once.
    bool phase_after(PhaseHandle phase, PhaseHandle other);
    bool phase_before(PhaseHandle phase, PhaseHandle other);

    // --- Systems --------------------------------------------------------------

    // A system of `phase`, enabled. Invalid (with a message) if the phase is
    // stale, `fn` is null or the name is empty, too long or taken in that
    // phase.
    SystemHandle add_system(PhaseHandle phase, const char* name, SystemFn fn, void* user_data);
    SystemHandle find_system(PhaseHandle phase, const char* name) const;
    // Removes the system and every constraint naming it. Allowed from
    // inside a run (the system is skipped if it has not run yet).
    bool remove_system(SystemHandle handle);
    SystemRecord* system(SystemHandle handle);
    const SystemRecord* system(SystemHandle handle) const;
    // `system` runs after / before `other`. False for a stale handle, a
    // system against itself or one of another phase; a constraint given
    // twice is kept once.
    bool system_after(SystemHandle system, SystemHandle other);
    bool system_before(SystemHandle system, SystemHandle other);
    // A disabled system stays in the order and is skipped. Not structural.
    bool set_enabled(SystemHandle system, bool enabled);
    bool is_enabled(SystemHandle system) const;

    // --- Running --------------------------------------------------------------

    // Calls every enabled system of `pipeline`, phase by phase, in the
    // compiled order. False (nothing run) for a stale handle or a broken
    // pipeline.
    bool run(PipelineHandle pipeline, void* host, void* argument, f32 dt);

    // Rebuilds the order of every pipeline if `dirty`. False if any pipeline
    // is broken afterwards (each reported once, when found).
    bool compile();
    // The phases of `pipeline` in run order, appended to `out`. False (nothing
    // appended) for a stale handle or a broken pipeline.
    bool phase_order(PipelineHandle pipeline, DynamicArray<PhaseHandle>& out);
    // The systems of `phase` in run order, appended to `out`. False (nothing
    // appended) for a stale handle or a broken pipeline.
    bool system_order(PhaseHandle phase, DynamicArray<SystemHandle>& out);

    // Marks every pipeline for recompilation; the structural edits call it.
    void mark_dirty();

    // --- Members --------------------------------------------------------------

    BaseAllocator* allocator = nullptr;
    SparseList<PipelineRecord> pipelines;
    SparseList<PhaseRecord> phases;
    SparseList<SystemRecord> systems;
    // "first runs after second", both of one pipeline.
    struct PhaseEdge {
        PhaseHandle phase;
        PhaseHandle after;
    };
    DynamicArray<PhaseEdge> phase_edges;
    // "first runs after second", both of one phase.
    struct SystemEdge {
        SystemHandle system;
        SystemHandle after;
    };
    DynamicArray<SystemEdge> system_edges;
    // The compiled order of every pipeline, one slice each
    // (PipelineRecord::order_first / order_count).
    DynamicArray<ScheduleEntry> order;
    bool dirty = true;
    u64 next_sequence = 0;

private:
    void report(const char* format, ...);
    // Orders `pipeline`'s phases and systems into `order`; false (reported)
    // on a cycle.
    bool compile_pipeline(PipelineHandle handle, PipelineRecord& record);
};
