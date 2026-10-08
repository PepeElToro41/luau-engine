#include "engine/ecs/query_vm.hpp"

#include "engine/ecs/archetype.hpp"
#include "engine/ecs/archetype_candidates.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"

#include <cstdio>

namespace QUERY_VM {

namespace {

constexpr usz NO_COLUMN = static_cast<usz>(-1);

// What a variable is bound to. THIS bound by SELECT is a whole archetype
// (entity 0, row 0); THIS bound by begin_for() and every other variable are
// one entity: its archetype and row. Unbound when archetype is nullptr.
struct Binding {
    EntityId entity = 0;
    Archetype* archetype = nullptr;
    usz row = 0;
};

// Where an op is in its own iteration, kept across the passes that resume it.
struct OpState {
    // SELECT: index into the candidate list.
    usz cursor = 0;
    // AND / UP: the column matched last, where the next pass resumes from.
    usz column = NO_COLUMN;
    // UP: the ancestor found for the current source.
    Archetype* archetype = nullptr;
    usz row = 0;
    EntityId entity = 0;
    // Optional ops: whether anything matched for the current source, and
    // whether the "nothing matched" result was already given.
    bool matched = false;
    bool none_given = false;
};

// A plain THIS term as the trivial mode fills it: where to write, what to
// look for, and whether the candidate list already names its column.
struct TrivialTerm {
    u32 term = 0;
    u32 field = QUERY_OP_NONE;
    Id pattern = 0;
    bool is_record_term = false;
};

struct VmState {
    World* world = nullptr;
    const QueryProgram* program = nullptr;

    Binding* vars = nullptr;
    OpState* op_states = nullptr;

    // SELECT's archetypes: the narrowed candidate list, or nullptr for a
    // walk over the world's alive archetypes with `candidate_count` as the
    // ceiling (see QUERY_SCAN for the same scheme).
    ArchetypeCandidate* candidates = nullptr;
    usz candidate_count = 0;
    // The plain THIS term whose record the candidates came from, or
    // QUERY_OP_NONE: the candidate already names its column, so YIELD need
    // not look it up. `candidate_column` is that column for the archetype
    // SELECT bound last, NO_COLUMN when it came from elsewhere.
    u32 record_term = QUERY_OP_NONE;
    usz candidate_column = NO_COLUMN;
    // begin_for(): THIS is this entity alone.
    EntityId this_entity = 0;
    // Trivial mode (program is SELECT + YIELD, THIS not pre-bound): the
    // terms to fill per archetype, prepared once by begin(). Everything
    // else in the chunk (sources, shared flags, excluded ids, vars) never
    // changes and is written once.
    TrivialTerm* trivial_terms = nullptr;
    usz trivial_count = 0;

    // Where the program resumes: the op, and whether it is resumed (redo)
    // rather than entered.
    usz op = 0;
    bool redo = false;
    bool done = false;

    // Chunk buffers the QueryIter points into.
    void** columns = nullptr;
    bool* shared = nullptr;
    Id* ids = nullptr;
    EntityId* sources = nullptr;
    EntityId* var_entities = nullptr;
};

// --- Helpers -----------------------------------------------------------------

bool is_bound(const VmState* state, const QueryVar var) {
    return var.is_set() && state->vars[var.index].archetype != nullptr;
}

void unbind(VmState* state, const u32 var) {
    if (var != QUERY_OP_NONE) {
        state->vars[var] = Binding { };
    }
}

// The term's id with every bound variable side substituted. Sides the op
// itself binds are left as WILDCARD even once bound, since the op is the one
// iterating over them.
Id resolve_id(const VmState* state, const QueryTerm& term, const QueryOp* op) {
    Id id = ECS::FOLD_ANY(term.id);
    if (!ECS::IS_PAIR(id)) {
        return id;
    }
    EntityIdLow first = ECS::PAIR_FIRST(id);
    EntityIdLow second = ECS::PAIR_SECOND(id);
    const bool op_binds_first = op != nullptr && op->bind_first != QUERY_OP_NONE;
    const bool op_binds_second = op != nullptr && op->bind_second != QUERY_OP_NONE;
    if (!op_binds_first && is_bound(state, term.first_var)) {
        first = ECS::ENTITY_LOW(state->vars[term.first_var.index].entity);
    }
    if (!op_binds_second && is_bound(state, term.second_var)) {
        second = ECS::ENTITY_LOW(state->vars[term.second_var.index].entity);
    }
    return ECS::PAIR(first, second);
}

// The term's source: the bound variable, or the fixed entity looked up
// live. False if the fixed entity is not alive.
bool source_of(const VmState* state, const QueryTerm& term, Binding* out) {
    if (term.src_var.is_set()) {
        *out = state->vars[term.src_var.index];
        return out->archetype != nullptr;
    }
    const EntityRecord* record = state->world->entity_index.get_record_alive(term.src);
    if (record == nullptr || record->archetype == nullptr) {
        return false;
    }
    out->entity = term.src;
    out->archetype = record->archetype;
    out->row = record->archetype_row;
    return true;
}

// Index of the first id of `archetype` at or after `start` matching
// `pattern`, or NO_COLUMN. From the start, the column maps answer at once
// for concrete ids and the (R, *) / (*, T) aliases.
usz find_column(const Archetype* archetype, const Id pattern, const usz start) {
    const ArchetypeType& type = archetype->type;
    if (start == 0) {
        const usz* index = archetype->columns_index.find(pattern);
        if (index != nullptr) {
            return *index;
        }
    }
    for (usz i = start; i < type.id_count; i++) {
        if (ECS::ID_MATCHES(pattern, type.ids[i])) {
            return i;
        }
    }
    return NO_COLUMN;
}

// Records what term `index` matched: the id, the source and, for an output,
// the data pointer. `self` is true when the data belongs to the chunk's own
// rows (THIS), false when the field is one element shared by every row.
void write_result(VmState* state, const u32 index, const Archetype* archetype, const usz row, const usz column, const EntityId source, const bool self) {
    const QueryProgram* program = state->program;
    state->ids[index] = column != NO_COLUMN ? archetype->type.ids[column] : 0;
    state->sources[index] = source;

    const u32 field = program->term_fields[index];
    if (field == QUERY_OP_NONE) {
        return;
    }
    state->shared[field] = !self;
    if (column == NO_COLUMN) {
        state->columns[field] = nullptr;
        return;
    }
    const ArchetypeColumn& data = archetype->data.columns[column];
    // Tags store nothing.
    state->columns[field] = data.type_info.length != 0 ? data.read(row) : nullptr;
}

void write_none(VmState* state, const u32 index) {
    write_result(state, index, nullptr, 0, NO_COLUMN, 0, true);
}

// Binds the op's variables from `id`. False, with nothing bound, if a side
// is not an alive entity.
bool bind_from_id(VmState* state, const QueryOp& op, const Id id) {
    World* world = state->world;
    const u32 targets[2] = { op.bind_first, op.bind_second };
    for (usz side = 0; side < 2; side++) {
        const u32 var = targets[side];
        if (var == QUERY_OP_NONE) {
            continue;
        }
        EntityId entity;
        const EntityRecord* record;
        if (!ECS::IS_PAIR(id)) {
            entity = id;
            record = world->entity_index.get_record_alive(entity);
        } else {
            record = world->entity_index.resolve_low(side == 0 ? ECS::PAIR_FIRST(id) : ECS::PAIR_SECOND(id), &entity);
        }
        if (record == nullptr || record->archetype == nullptr) {
            unbind(state, op.bind_first);
            unbind(state, op.bind_second);
            return false;
        }
        state->vars[var].entity = entity;
        state->vars[var].archetype = record->archetype;
        state->vars[var].row = record->archetype_row;
    }
    return true;
}

// Depth-first over the (relation, *) pairs of `archetype`: the first
// ancestor whose archetype holds an id matching `pattern`. The pairs of one
// relation are contiguous in the sorted type, starting at the (R, *) alias.
bool traverse_up(World* world, const Archetype* archetype, const EntityIdLow relation, const Id pattern, const usz depth, Binding* out) {
    if (depth >= MAX_TRAVERSAL_DEPTH) {
        fprintf(stderr, "[ecs] error: query up(%llx) walked %llu levels; the relation has a cycle\n",
            static_cast<unsigned long long>(relation), static_cast<unsigned long long>(depth));
        return false;
    }
    const ArchetypeType& type = archetype->type;
    const usz* first = archetype->columns_index.find(ECS::PAIR(relation, ECS::WILDCARD));
    if (first == nullptr) {
        return false;
    }
    for (usz i = *first; i < type.id_count && ECS::PAIR_FIRST(type.ids[i]) == relation; i++) {
        EntityId target;
        const EntityRecord* record = world->entity_index.resolve_low(ECS::PAIR_SECOND(type.ids[i]), &target);
        if (record == nullptr || record->archetype == nullptr) {
            continue;
        }
        if (find_column(record->archetype, pattern, 0) != NO_COLUMN) {
            out->entity = target;
            out->archetype = record->archetype;
            out->row = record->archetype_row;
            return true;
        }
        if (traverse_up(world, record->archetype, relation, pattern, depth + 1, out)) {
            return true;
        }
    }
    return false;
}

// --- Ops ---------------------------------------------------------------------

// The next candidate archetype that is non-empty and passes the matcher,
// advancing `cursor`; nullptr when there are none left. `out_column` gets
// the column the candidate list names, NO_COLUMN on the dense-list walk.
Archetype* select_next(VmState* state, usz& cursor, usz* out_column) {
    const QueryProgram* program = state->program;
    World* world = state->world;
    const SparseList<Archetype>& archetypes = world->archetypes;
    while (cursor < state->candidate_count) {
        Archetype* archetype;
        usz column = NO_COLUMN;
        if (state->candidates != nullptr) {
            const ArchetypeCandidate& candidate = state->candidates[cursor];
            archetype = ARCHETYPE_CANDIDATES::archetype_of(world, candidate);
            cursor++;
            if (archetype == nullptr) {
                continue;
            }
            column = candidate.column;
        } else {
            if (cursor >= archetypes.alive_count) {
                break;
            }
            archetype = archetypes.get_element_any(archetypes.get_alive_id(cursor));
            cursor++;
        }
        if (archetype->data.entity_count == 0 || !program->matcher.matches(archetype)) {
            continue;
        }
        *out_column = column;
        return archetype;
    }
    return nullptr;
}

bool eval_select(VmState* state, OpState& st, const bool redo) {
    const QueryProgram* program = state->program;
    World* world = state->world;
    Binding& self = state->vars[QUERY_THIS.index];

    state->candidate_column = NO_COLUMN;
    if (state->this_entity != 0) {
        if (redo) {
            self = Binding { };
            return false;
        }
        const EntityRecord* record = world->entity_index.get_record_alive(state->this_entity);
        if (record == nullptr || record->archetype == nullptr || record->archetype == world->root_archetype) {
            return false;
        }
        if (!program->matcher.matches(record->archetype)) {
            return false;
        }
        self.entity = state->this_entity;
        self.archetype = record->archetype;
        self.row = record->archetype_row;
        return true;
    }

    if (!redo) {
        st.cursor = 0;
    }
    usz column = NO_COLUMN;
    Archetype* archetype = select_next(state, st.cursor, &column);
    if (archetype == nullptr) {
        self = Binding { };
        return false;
    }
    self.entity = 0;
    self.archetype = archetype;
    self.row = 0;
    state->candidate_column = column;
    return true;
}

// One term against one archetype: finds the next column matching the term
// and, when the op binds variables, binds them from it. `redo` resumes
// after the column matched last.
bool eval_match(VmState* state, const QueryOp& op, OpState& st, const Archetype* archetype, const usz row, const EntityId source, const bool self, const bool redo) {
    const QueryTerm& term = state->program->terms[op.term];
    const bool binds = op.bind_first != QUERY_OP_NONE || op.bind_second != QUERY_OP_NONE;
    if (redo && !binds) {
        return false;
    }
    const Id pattern = resolve_id(state, term, &op);
    usz start = redo ? st.column + 1 : 0;
    while (true) {
        const usz column = find_column(archetype, pattern, start);
        if (column == NO_COLUMN) {
            return false;
        }
        if (!binds || bind_from_id(state, op, archetype->type.ids[column])) {
            st.column = column;
            write_result(state, op.term, archetype, row, column, source, self);
            return true;
        }
        start = column + 1;
    }
}

// The tail shared by AND and UP once the match itself is exhausted: unbind,
// and give an optional term its one empty result.
bool exhausted(VmState* state, const QueryOp& op, OpState& st) {
    unbind(state, op.bind_first);
    unbind(state, op.bind_second);
    if (op.optional && !st.matched) {
        st.matched = true;
        st.none_given = true;
        write_none(state, op.term);
        return true;
    }
    return false;
}

bool eval_and(VmState* state, const QueryOp& op, OpState& st, const bool redo) {
    const QueryTerm& term = state->program->terms[op.term];
    if (!redo) {
        st.matched = false;
        st.none_given = false;
    } else if (st.none_given) {
        return false;
    }

    Binding source;
    bool ok = false;
    if (source_of(state, term, &source)) {
        const bool self = term.src_var == QUERY_THIS;
        ok = eval_match(state, op, st, source.archetype, source.row, self ? 0 : source.entity, self, redo);
    }
    if (ok) {
        st.matched = true;
        return true;
    }
    return exhausted(state, op, st);
}

bool eval_not(VmState* state, const QueryOp& op, const bool redo) {
    if (redo) {
        return false;
    }
    const QueryTerm& term = state->program->terms[op.term];
    Binding source;
    if (source_of(state, term, &source)) {
        const Id pattern = resolve_id(state, term, &op);
        if (find_column(source.archetype, pattern, 0) != NO_COLUMN) {
            return false;
        }
    }
    // A source that is not alive holds nothing, so the term holds.
    write_none(state, op.term);
    return true;
}

bool eval_up(VmState* state, const QueryOp& op, OpState& st, const bool redo) {
    const QueryTerm& term = state->program->terms[op.term];
    if (!redo) {
        st.matched = false;
        st.none_given = false;
        st.archetype = nullptr;
        Binding source;
        if (source_of(state, term, &source)) {
            const Id pattern = resolve_id(state, term, &op);
            Binding ancestor;
            if (traverse_up(state->world, source.archetype, ECS::ENTITY_LOW(term.traverse), pattern, 0, &ancestor)) {
                st.archetype = ancestor.archetype;
                st.row = ancestor.row;
                st.entity = ancestor.entity;
            }
        }
    } else if (st.none_given) {
        return false;
    }

    if (term.is_excluded()) {
        if (redo || st.archetype != nullptr) {
            return false;
        }
        write_none(state, op.term);
        return true;
    }

    bool ok = false;
    if (st.archetype != nullptr) {
        ok = eval_match(state, op, st, st.archetype, st.row, st.entity, false, redo);
    }
    if (ok) {
        st.matched = true;
        return true;
    }
    return exhausted(state, op, st);
}

// Whether alternative `index` of an or-chain holds, recording its result.
bool eval_alternative(VmState* state, const u32 index) {
    const QueryTerm& term = state->program->terms[index];
    Binding source;
    Binding where;
    bool have_where = false;
    if (source_of(state, term, &source)) {
        const Id pattern = resolve_id(state, term, nullptr);
        if (term.traverses()) {
            have_where = traverse_up(state->world, source.archetype, ECS::ENTITY_LOW(term.traverse), pattern, 0, &where);
        } else {
            where = source;
            have_where = true;
        }
        if (have_where) {
            const usz column = find_column(where.archetype, pattern, 0);
            if (column != NO_COLUMN) {
                if (term.is_excluded()) {
                    return false;
                }
                const bool self = term.src_var == QUERY_THIS && !term.traverses();
                write_result(state, index, where.archetype, where.row, column, self ? 0 : where.entity, self);
                return true;
            }
        }
    }
    if (term.is_excluded()) {
        write_none(state, index);
        return true;
    }
    return false;
}

bool eval_or(VmState* state, const QueryOp& op, const bool redo) {
    if (redo) {
        return false;
    }
    bool held = false;
    for (u32 i = op.term; i < op.term + op.term_count; i++) {
        if (held) {
            write_none(state, i);
        } else if (eval_alternative(state, i)) {
            held = true;
        } else {
            write_none(state, i);
        }
    }
    return held;
}

// Fills the chunk from the bindings: THIS rows, the plain terms on THIS,
// and the variables.
void fill_chunk(QueryIter* it, VmState* state) {
    const QueryProgram* program = state->program;
    const Binding& self = state->vars[QUERY_THIS.index];

    if (self.archetype != nullptr) {
        it->archetype = self.archetype;
        it->entities = self.archetype->data.entities + self.row;
        // A whole archetype reads its live row count; one entity is one row.
        it->count = self.entity == 0 ? self.archetype->data.entity_count : 1;
    } else {
        it->archetype = nullptr;
        it->entities = nullptr;
        it->count = 0;
    }

    for (usz i = 0; i < program->this_term_count; i++) {
        const u32 index = program->this_terms[i];
        const QueryTerm& term = program->terms[index];
        if (term.is_excluded() || self.archetype == nullptr) {
            write_none(state, index);
            continue;
        }
        const usz column = index == state->record_term && state->candidate_column != NO_COLUMN
            ? state->candidate_column
            : find_column(self.archetype, ECS::FOLD_ANY(term.id), 0);
        write_result(state, index, self.archetype, self.row, column, 0, true);
    }

    for (usz i = 0; i < program->var_count; i++) {
        state->var_entities[i] = state->vars[i].entity;
    }
}

bool eval(VmState* state, const usz op_index, const bool redo) {
    const QueryOp& op = state->program->ops[op_index];
    OpState& st = state->op_states[op_index];
    switch (op.kind) {
        case QUERY_OP_SELECT: return eval_select(state, st, redo);
        case QUERY_OP_AND: return eval_and(state, op, st, redo);
        case QUERY_OP_NOT: return eval_not(state, op, redo);
        case QUERY_OP_UP: return eval_up(state, op, st, redo);
        case QUERY_OP_OR: return eval_or(state, op, redo);
        case QUERY_OP_YIELD: return !redo;
    }
    return false;
}

void clear_chunk(QueryIter* it) {
    it->archetype = nullptr;
    it->entities = nullptr;
    it->count = 0;
}

bool next(QueryIter* it) {
    VmState* state = static_cast<VmState*>(it->state);
    if (state->done) {
        clear_chunk(it);
        return false;
    }

    usz op = state->op;
    bool redo = state->redo;
    while (true) {
        if (eval(state, op, redo)) {
            if (state->program->ops[op].kind == QUERY_OP_YIELD) {
                fill_chunk(it, state);
                state->op = op;
                state->redo = true;
                return true;
            }
            op++;
            redo = false;
            continue;
        }
        if (op == 0) {
            state->done = true;
            clear_chunk(it);
            return false;
        }
        op--;
        redo = true;
    }
}

bool next_nothing(QueryIter* it) {
    clear_chunk(it);
    return false;
}

// Trivial mode: the whole program is "every candidate the matcher accepts",
// so there is nothing to dispatch or backtrack. One archetype per call,
// filled from the prepared term list.
bool next_trivial(QueryIter* it) {
    VmState* state = static_cast<VmState*>(it->state);
    if (state->done) {
        clear_chunk(it);
        return false;
    }
    usz record_column = NO_COLUMN;
    Archetype* archetype = select_next(state, state->op_states[0].cursor, &record_column);
    if (archetype == nullptr) {
        state->done = true;
        clear_chunk(it);
        return false;
    }

    it->archetype = archetype;
    it->entities = archetype->data.entities;
    it->count = archetype->data.entity_count;
    for (usz i = 0; i < state->trivial_count; i++) {
        const TrivialTerm& term = state->trivial_terms[i];
        const usz column = term.is_record_term && record_column != NO_COLUMN
            ? record_column
            : find_column(archetype, term.pattern, 0);
        if (column == NO_COLUMN) {
            // Only an optional term gets here: the matcher vouched for the rest.
            state->ids[term.term] = 0;
            if (term.field != QUERY_OP_NONE) {
                state->columns[term.field] = nullptr;
            }
            continue;
        }
        state->ids[term.term] = archetype->type.ids[column];
        if (term.field != QUERY_OP_NONE) {
            const ArchetypeColumn& data = archetype->data.columns[column];
            state->columns[term.field] = data.type_info.length != 0 ? data.data : nullptr;
        }
    }
    return true;
}

// Prepares the trivial mode's term list; the parts of the chunk that never
// change are already in their initial state (ids 0, sources 0, shared
// false, vars 0).
void prepare_trivial(VmState* state, BaseAllocator* allocator) {
    const QueryProgram* program = state->program;
    state->trivial_terms = allocator->allocate_array<TrivialTerm>(program->this_term_count + 1);
    state->trivial_count = 0;
    for (usz i = 0; i < program->this_term_count; i++) {
        const u32 index = program->this_terms[i];
        const QueryTerm& term = program->terms[index];
        if (term.is_excluded()) {
            continue;
        }
        TrivialTerm& out = state->trivial_terms[state->trivial_count++];
        out.term = index;
        out.field = program->term_fields[index];
        out.pattern = ECS::FOLD_ANY(term.id);
        out.is_record_term = index == state->record_term;
    }
}

// Whether the program has nothing for the VM to do beyond SELECT + YIELD.
bool is_trivial(const QueryProgram* program) {
    return program->op_count == 2 && program->ops[0].kind == QUERY_OP_SELECT && program->ops[1].kind == QUERY_OP_YIELD;
}

QueryIter start(World* world, const QueryProgram* program, const EntityId entity, BaseAllocator* allocator) {
    QueryIter it;
    it.world = world;
    it.next = next_nothing;
    if (!program->ok) {
        return it;
    }

    VmState* state = allocator->allocate_array<VmState>(1);
    *state = VmState { };
    state->world = world;
    state->program = program;
    state->this_entity = entity;

    state->vars = allocator->allocate_array<Binding>(program->var_count + 1);
    state->var_entities = allocator->allocate_array<EntityId>(program->var_count + 1);
    for (usz i = 0; i < program->var_count; i++) {
        state->vars[i] = Binding { };
        state->var_entities[i] = 0;
    }
    state->op_states = allocator->allocate_array<OpState>(program->op_count + 1);
    for (usz i = 0; i < program->op_count; i++) {
        state->op_states[i] = OpState { };
    }
    state->ids = allocator->allocate_array<Id>(program->term_count + 1);
    state->sources = allocator->allocate_array<EntityId>(program->term_count + 1);
    for (usz i = 0; i < program->term_count; i++) {
        state->ids[i] = 0;
        state->sources[i] = 0;
    }
    state->columns = allocator->allocate_array<void*>(program->field_count + 1);
    state->shared = allocator->allocate_array<bool>(program->field_count + 1);
    for (usz i = 0; i < program->field_count; i++) {
        state->columns[i] = nullptr;
        state->shared[i] = false;
    }

    if (program->binds_this && entity == 0) {
        const ArchetypeCandidates candidates = ARCHETYPE_CANDIDATES::collect(world, program->with_ids, program->with_count, allocator);
        if (candidates.narrowed) {
            state->candidates = candidates.entries;
            state->candidate_count = candidates.count;
            for (usz i = 0; i < program->this_term_count; i++) {
                const QueryTerm& term = program->terms[program->this_terms[i]];
                if (!term.is_excluded() && ECS::FOLD_ANY(term.id) == candidates.record_id) {
                    state->record_term = program->this_terms[i];
                    break;
                }
            }
        } else {
            state->candidates = nullptr;
            state->candidate_count = world->archetypes.alive_count;
        }
    }

    it.columns = state->columns;
    it.shared = state->shared;
    it.field_count = program->field_count;
    it.ids = state->ids;
    it.sources = state->sources;
    it.term_count = program->term_count;
    it.vars = state->var_entities;
    it.var_count = program->var_count;
    it.state = state;
    if (entity == 0 && is_trivial(program)) {
        prepare_trivial(state, allocator);
        it.next = next_trivial;
    } else {
        it.next = next;
    }
    return it;
}

} // namespace

QueryIter begin(World* world, const QueryProgram* program, BaseAllocator* allocator) {
    return start(world, program, 0, allocator);
}

QueryIter begin_for(World* world, const QueryProgram* program, const EntityId entity, BaseAllocator* allocator) {
    if (entity == 0) {
        QueryIter it;
        it.world = world;
        it.next = next_nothing;
        return it;
    }
    return start(world, program, entity, allocator);
}

bool matches(World* world, const QueryProgram* program, const EntityId entity) {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = begin_for(world, program, entity, &temp);
    return it.next(&it);
}

} // namespace QUERY_VM
