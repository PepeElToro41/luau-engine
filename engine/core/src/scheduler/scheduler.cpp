#include "engine/scheduler/scheduler.hpp"

#include "engine/memory/heap_allocator.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/utils/hash.hpp"
#include "engine/utils/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

// --- Names ---------------------------------------------------------------------

// Copies `name` into a NAME_MAX buffer. False if empty or too long.
static bool copy_name(char* out, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    const usz length = strlen(name);
    if (length >= SCHEDULER::NAME_MAX) {
        return false;
    }
    memcpy(out, name, length + 1);
    return true;
}

static const char* name_or(const char* name, const char* fallback) {
    return name != nullptr ? name : fallback;
}

// --- Lifetime ------------------------------------------------------------------

void Scheduler::init(BaseAllocator* allocator) {
    if (allocator == nullptr) {
        allocator = MEMORY::heap_allocator();
    }
    this->free();
    this->allocator = allocator;
    this->pipelines = SparseList<PipelineRecord>(allocator);
    this->phases = SparseList<PhaseRecord>(allocator);
    this->systems = SparseList<SystemRecord>(allocator);
    this->phase_edges = DynamicArray<PhaseEdge>(allocator);
    this->system_edges = DynamicArray<SystemEdge>(allocator);
    this->order = DynamicArray<ScheduleEntry>(allocator);
    this->dirty = true;
    this->next_sequence = 0;
}

void Scheduler::free() {
    this->pipelines.free();
    this->phases.free();
    this->systems.free();
    this->phase_edges.free();
    this->system_edges.free();
    this->order.free();
    this->dirty = true;
    this->next_sequence = 0;
}

void Scheduler::mark_dirty() {
    this->dirty = true;
}

void Scheduler::report(const char* format, ...) {
    va_list args;
    va_start(args, format);
    LOG::vwrite(LOG_ERROR, LOG_CATEGORY::SCHEDULER, nullptr, 0, format, args);
    va_end(args);
}

// --- Pipelines -----------------------------------------------------------------

PipelineHandle Scheduler::add_pipeline(const char* name, const PipelineOrder order) {
    char copied[SCHEDULER::NAME_MAX];
    if (!copy_name(copied, name)) {
        this->report("pipeline '%s': the name is empty or longer than %u characters", name_or(name, ""), SCHEDULER::NAME_MAX - 1);
        return PipelineHandle{};
    }
    if (this->find_pipeline(copied).is_valid()) {
        this->report("pipeline '%s' already exists", copied);
        return PipelineHandle{};
    }
    PipelineHandle handle;
    handle.id = this->pipelines.new_element();
    PipelineRecord* record = this->pipelines.get_element_alive(handle.id);
    memcpy(record->name, copied, sizeof(copied));
    record->name_hash = HASH::fnv1a_str(copied);
    record->order = order;
    record->sequence = this->next_sequence++;
    this->mark_dirty();
    return handle;
}

PipelineHandle Scheduler::find_pipeline(const char* name) const {
    if (name == nullptr) {
        return PipelineHandle{};
    }
    const u64 hash = HASH::fnv1a_str(name);
    for (usz i = 0; i < this->pipelines.alive_count; ++i) {
        const SparseId id = this->pipelines.get_alive_id(i);
        const PipelineRecord* record = this->pipelines.get_element_any(id);
        if (record->name_hash == hash && strcmp(record->name, name) == 0) {
            PipelineHandle handle;
            handle.id = id;
            return handle;
        }
    }
    return PipelineHandle{};
}

PipelineRecord* Scheduler::pipeline(const PipelineHandle handle) {
    return handle.is_valid() ? this->pipelines.get_element_alive(handle.id) : nullptr;
}

const PipelineRecord* Scheduler::pipeline(const PipelineHandle handle) const {
    return handle.is_valid() ? this->pipelines.get_element_alive(handle.id) : nullptr;
}

// --- Phases --------------------------------------------------------------------

PhaseHandle Scheduler::add_phase(const PipelineHandle pipeline, const char* name) {
    const PipelineRecord* owner = this->pipeline(pipeline);
    if (owner == nullptr) {
        this->report("phase '%s': the pipeline handle is stale", name_or(name, ""));
        return PhaseHandle{};
    }
    char copied[SCHEDULER::NAME_MAX];
    if (!copy_name(copied, name)) {
        this->report("pipeline '%s': a phase name is empty or longer than %u characters", owner->name, SCHEDULER::NAME_MAX - 1);
        return PhaseHandle{};
    }
    if (this->find_phase(pipeline, copied).is_valid()) {
        this->report("pipeline '%s' already has a phase '%s'", owner->name, copied);
        return PhaseHandle{};
    }
    PhaseHandle handle;
    handle.id = this->phases.new_element();
    PhaseRecord* record = this->phases.get_element_alive(handle.id);
    memcpy(record->name, copied, sizeof(copied));
    record->name_hash = HASH::fnv1a_str(copied);
    record->pipeline = pipeline;
    record->sequence = this->next_sequence++;
    this->mark_dirty();
    return handle;
}

PhaseHandle Scheduler::find_phase(const PipelineHandle pipeline, const char* name) const {
    if (name == nullptr || this->pipeline(pipeline) == nullptr) {
        return PhaseHandle{};
    }
    const u64 hash = HASH::fnv1a_str(name);
    for (usz i = 0; i < this->phases.alive_count; ++i) {
        const SparseId id = this->phases.get_alive_id(i);
        const PhaseRecord* record = this->phases.get_element_any(id);
        if (record->pipeline == pipeline && record->name_hash == hash && strcmp(record->name, name) == 0) {
            PhaseHandle handle;
            handle.id = id;
            return handle;
        }
    }
    return PhaseHandle{};
}

bool Scheduler::remove_phase(const PhaseHandle handle) {
    const PhaseRecord* record = this->phase(handle);
    if (record == nullptr) {
        return false;
    }
    for (usz i = 0; i < this->systems.alive_count; ++i) {
        const SystemRecord* system = this->systems.get_element_any(this->systems.get_alive_id(i));
        if (system->phase == handle) {
            this->report("phase '%s' still holds system '%s'; remove its systems first", record->name, system->name);
            return false;
        }
    }
    for (usz i = this->phase_edges.count; i-- > 0;) {
        if (this->phase_edges[i].phase == handle || this->phase_edges[i].after == handle) {
            this->phase_edges.remove_swap(i);
        }
    }
    this->phases.delete_element(handle.id);
    this->mark_dirty();
    return true;
}

PhaseRecord* Scheduler::phase(const PhaseHandle handle) {
    return handle.is_valid() ? this->phases.get_element_alive(handle.id) : nullptr;
}

const PhaseRecord* Scheduler::phase(const PhaseHandle handle) const {
    return handle.is_valid() ? this->phases.get_element_alive(handle.id) : nullptr;
}

bool Scheduler::phase_after(const PhaseHandle phase, const PhaseHandle other) {
    const PhaseRecord* record = this->phase(phase);
    const PhaseRecord* other_record = this->phase(other);
    if (record == nullptr || other_record == nullptr) {
        return false;
    }
    if (phase == other) {
        this->report("phase '%s' cannot be ordered against itself", record->name);
        return false;
    }
    if (record->pipeline != other_record->pipeline) {
        this->report("phases '%s' and '%s' are of different pipelines and cannot be ordered", record->name, other_record->name);
        return false;
    }
    for (const PhaseEdge& edge : this->phase_edges) {
        if (edge.phase == phase && edge.after == other) {
            return true;
        }
    }
    this->phase_edges.push(PhaseEdge{phase, other});
    this->mark_dirty();
    return true;
}

bool Scheduler::phase_before(const PhaseHandle phase, const PhaseHandle other) {
    return this->phase_after(other, phase);
}

// --- Systems -------------------------------------------------------------------

SystemHandle Scheduler::add_system(const PhaseHandle phase, const char* name, const SystemFn fn, void* user_data) {
    const PhaseRecord* owner = this->phase(phase);
    if (owner == nullptr) {
        this->report("system '%s': the phase handle is stale", name_or(name, ""));
        return SystemHandle{};
    }
    char copied[SCHEDULER::NAME_MAX];
    if (!copy_name(copied, name)) {
        this->report("phase '%s': a system name is empty or longer than %u characters", owner->name, SCHEDULER::NAME_MAX - 1);
        return SystemHandle{};
    }
    if (fn == nullptr) {
        this->report("system '%s' has no function", copied);
        return SystemHandle{};
    }
    if (this->find_system(phase, copied).is_valid()) {
        this->report("phase '%s' already has a system '%s'", owner->name, copied);
        return SystemHandle{};
    }
    SystemHandle handle;
    handle.id = this->systems.new_element();
    SystemRecord* record = this->systems.get_element_alive(handle.id);
    memcpy(record->name, copied, sizeof(copied));
    record->name_hash = HASH::fnv1a_str(copied);
    record->phase = phase;
    record->pipeline = owner->pipeline;
    record->fn = fn;
    record->user_data = user_data;
    record->sequence = this->next_sequence++;
    record->enabled = true;
    this->mark_dirty();
    return handle;
}

SystemHandle Scheduler::find_system(const PhaseHandle phase, const char* name) const {
    if (name == nullptr || this->phase(phase) == nullptr) {
        return SystemHandle{};
    }
    const u64 hash = HASH::fnv1a_str(name);
    for (usz i = 0; i < this->systems.alive_count; ++i) {
        const SparseId id = this->systems.get_alive_id(i);
        const SystemRecord* record = this->systems.get_element_any(id);
        if (record->phase == phase && record->name_hash == hash && strcmp(record->name, name) == 0) {
            SystemHandle handle;
            handle.id = id;
            return handle;
        }
    }
    return SystemHandle{};
}

bool Scheduler::remove_system(const SystemHandle handle) {
    if (this->system(handle) == nullptr) {
        return false;
    }
    for (usz i = this->system_edges.count; i-- > 0;) {
        if (this->system_edges[i].system == handle || this->system_edges[i].after == handle) {
            this->system_edges.remove_swap(i);
        }
    }
    this->systems.delete_element(handle.id);
    this->mark_dirty();
    return true;
}

SystemRecord* Scheduler::system(const SystemHandle handle) {
    return handle.is_valid() ? this->systems.get_element_alive(handle.id) : nullptr;
}

const SystemRecord* Scheduler::system(const SystemHandle handle) const {
    return handle.is_valid() ? this->systems.get_element_alive(handle.id) : nullptr;
}

bool Scheduler::system_after(const SystemHandle system, const SystemHandle other) {
    const SystemRecord* record = this->system(system);
    const SystemRecord* other_record = this->system(other);
    if (record == nullptr || other_record == nullptr) {
        return false;
    }
    if (system == other) {
        this->report("system '%s' cannot be ordered against itself", record->name);
        return false;
    }
    if (record->phase != other_record->phase) {
        this->report("systems '%s' and '%s' are of different phases and cannot be ordered", record->name, other_record->name);
        return false;
    }
    for (const SystemEdge& edge : this->system_edges) {
        if (edge.system == system && edge.after == other) {
            return true;
        }
    }
    this->system_edges.push(SystemEdge{system, other});
    this->mark_dirty();
    return true;
}

bool Scheduler::system_before(const SystemHandle system, const SystemHandle other) {
    return this->system_after(other, system);
}

bool Scheduler::set_enabled(const SystemHandle system, const bool enabled) {
    SystemRecord* record = this->system(system);
    if (record == nullptr) {
        return false;
    }
    record->enabled = enabled;
    return true;
}

bool Scheduler::is_enabled(const SystemHandle system) const {
    const SystemRecord* record = this->system(system);
    return record != nullptr && record->enabled;
}

// --- Ordering ------------------------------------------------------------------

namespace {

// An item being ordered: a phase or a system by id, with its insertion
// sequence; `placed` once it has been given its position.
struct OrderItem {
    u64 id = 0;
    u64 sequence = 0;
    bool placed = false;
};

// Reorders `items` in place into run order: at every step the item that
// comes first by `order` among those whose dependencies are all placed.
// `pending(id)` says whether `id` still waits on an unplaced item. False
// when the remaining items all wait on each other (a cycle); the placed
// prefix is then items[0, placed_count), the cycle the rest.
template <typename Pending>
bool order_items(DynamicArray<OrderItem>& items, const PipelineOrder order, usz* placed_count, Pending pending) {
    for (usz placed = 0; placed < items.count; ++placed) {
        usz best = items.count;
        for (usz i = placed; i < items.count; ++i) {
            if (pending(items[i].id)) {
                continue;
            }
            if (best == items.count) {
                best = i;
            } else if (order == PIPELINE_ORDER_INSERTION ? items[i].sequence < items[best].sequence : items[i].sequence > items[best].sequence) {
                best = i;
            }
        }
        if (best == items.count) {
            *placed_count = placed;
            return false;
        }
        std::swap(items[placed], items[best]);
        items[placed].placed = true;
    }
    *placed_count = items.count;
    return true;
}

// Whether `id` is in `items` and not placed yet.
bool unplaced(const DynamicArray<OrderItem>& items, const u64 id) {
    for (const OrderItem& item : items) {
        if (item.id == id) {
            return !item.placed;
        }
    }
    return false;
}

// Appends the names of the unplaced items to `text`, for a cycle report.
template <typename NameOf>
void append_unplaced_names(char* text, const usz capacity, const DynamicArray<OrderItem>& items, NameOf name_of) {
    usz length = strlen(text);
    bool first = true;
    for (const OrderItem& item : items) {
        if (item.placed) {
            continue;
        }
        const int written = snprintf(text + length, capacity - length, "%s%s", first ? "" : ", ", name_of(item.id));
        if (written < 0 || static_cast<usz>(written) >= capacity - length) {
            break;
        }
        length += static_cast<usz>(written);
        first = false;
    }
}

} // namespace

bool Scheduler::compile_pipeline(const PipelineHandle handle, PipelineRecord& record) {
    TemporalAllocator temp = TemporalAllocator::create();

    // The pipeline's phases, in run order. Reserved for every alive phase
    // up front: an arena cannot grow a block, so a push past the capacity
    // would leave the old block dead in it until the rewind.
    DynamicArray<OrderItem> phase_items(&temp);
    phase_items.reserve(this->phases.alive_count);
    for (usz i = 0; i < this->phases.alive_count; ++i) {
        const SparseId id = this->phases.get_alive_id(i);
        const PhaseRecord* phase = this->phases.get_element_any(id);
        if (phase->pipeline == handle) {
            phase_items.push(OrderItem{id, phase->sequence, false});
        }
    }
    usz placed = 0;
    const bool phases_ordered = order_items(phase_items, record.order, &placed, [&](const u64 id) {
        for (const PhaseEdge& edge : this->phase_edges) {
            if (edge.phase.id == id && unplaced(phase_items, edge.after.id)) {
                return true;
            }
        }
        return false;
    });
    if (!phases_ordered) {
        char text[512];
        snprintf(text, sizeof(text), "pipeline '%s': its phases depend on each other in a cycle: ", record.name);
        append_unplaced_names(text, sizeof(text), phase_items, [&](const u64 id) { return this->phases.get_element_any(id)->name; });
        this->report("%s", text);
        return false;
    }

    // Each phase's systems, in run order, behind a marker entry for the
    // phase itself (so an empty phase still has its place in the order).
    DynamicArray<OrderItem> system_items(&temp);
    system_items.reserve(this->systems.alive_count);
    for (const OrderItem& phase_item : phase_items) {
        PhaseHandle phase;
        phase.id = phase_item.id;
        this->order.push(ScheduleEntry{SystemHandle{}, phase});

        system_items.clear();
        for (usz i = 0; i < this->systems.alive_count; ++i) {
            const SparseId id = this->systems.get_alive_id(i);
            const SystemRecord* system = this->systems.get_element_any(id);
            if (system->phase == phase) {
                system_items.push(OrderItem{id, system->sequence, false});
            }
        }
        const bool systems_ordered = order_items(system_items, record.order, &placed, [&](const u64 id) {
            for (const SystemEdge& edge : this->system_edges) {
                if (edge.system.id == id && unplaced(system_items, edge.after.id)) {
                    return true;
                }
            }
            return false;
        });
        if (!systems_ordered) {
            char text[512];
            snprintf(text, sizeof(text), "pipeline '%s', phase '%s': its systems depend on each other in a cycle: ", record.name,
                this->phases.get_element_any(phase.id)->name);
            append_unplaced_names(text, sizeof(text), system_items, [&](const u64 id) { return this->systems.get_element_any(id)->name; });
            this->report("%s", text);
            return false;
        }
        for (const OrderItem& system_item : system_items) {
            SystemHandle system;
            system.id = system_item.id;
            this->order.push(ScheduleEntry{system, phase});
        }
    }
    return true;
}

bool Scheduler::compile() {
    if (this->dirty) {
        this->order.clear();
        for (usz i = 0; i < this->pipelines.alive_count; ++i) {
            PipelineHandle handle;
            handle.id = this->pipelines.get_alive_id(i);
            PipelineRecord* record = this->pipelines.get_element_any(handle.id);
            record->order_first = this->order.count;
            record->broken = !this->compile_pipeline(handle, *record);
            if (record->broken) {
                // Nothing of a broken pipeline runs: drop the partial order.
                this->order.resize(record->order_first);
            }
            record->order_count = this->order.count - record->order_first;
        }
        this->dirty = false;
    }
    for (usz i = 0; i < this->pipelines.alive_count; ++i) {
        if (this->pipelines.get_element_any(this->pipelines.get_alive_id(i))->broken) {
            return false;
        }
    }
    return true;
}

bool Scheduler::phase_order(const PipelineHandle pipeline, DynamicArray<PhaseHandle>& out) {
    this->compile();
    const PipelineRecord* record = this->pipeline(pipeline);
    if (record == nullptr || record->broken) {
        return false;
    }
    for (usz i = 0; i < record->order_count; ++i) {
        const ScheduleEntry& entry = this->order[record->order_first + i];
        if (!entry.system.is_valid()) {
            out.push(entry.phase);
        }
    }
    return true;
}

bool Scheduler::system_order(const PhaseHandle phase, DynamicArray<SystemHandle>& out) {
    this->compile();
    const PhaseRecord* phase_record = this->phase(phase);
    if (phase_record == nullptr) {
        return false;
    }
    const PipelineRecord* record = this->pipeline(phase_record->pipeline);
    if (record == nullptr || record->broken) {
        return false;
    }
    for (usz i = 0; i < record->order_count; ++i) {
        const ScheduleEntry& entry = this->order[record->order_first + i];
        if (entry.phase == phase && entry.system.is_valid()) {
            out.push(entry.system);
        }
    }
    return true;
}

// --- Running -------------------------------------------------------------------

bool Scheduler::run(const PipelineHandle pipeline, void* host, void* argument, const f32 dt) {
    this->compile();
    const PipelineRecord* record = this->pipeline(pipeline);
    if (record == nullptr || record->broken) {
        return false;
    }
    // A copy: a system may edit the scheduler, or run another pipeline,
    // which rebuilds `order` under this walk.
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<ScheduleEntry> entries(&temp);
    entries.reserve(record->order_count);
    for (usz i = 0; i < record->order_count; ++i) {
        entries.push(this->order[record->order_first + i]);
    }
    for (const ScheduleEntry& entry : entries) {
        const SystemRecord* system = this->system(entry.system);
        if (system == nullptr || !system->enabled) {
            continue;
        }
        SystemContext ctx;
        ctx.scheduler = this;
        ctx.host = host;
        ctx.argument = argument;
        ctx.dt = dt;
        ctx.user_data = system->user_data;
        ctx.system = entry.system;
        ctx.phase = entry.phase;
        ctx.pipeline = pipeline;
        // The record may be gone once the function returns.
        const SystemFn fn = system->fn;
        fn(ctx);
    }
    return true;
}
