#include "engine/ecs/query/query_program.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/ecs/world.hpp"

#include <cstdio>

namespace QUERY_PROGRAM {

namespace {

// A term or an or-chain of terms: the thing one op evaluates.
struct Unit {
    u32 term = 0;
    u32 term_count = 1;
    bool is_or = false;
    bool done = false;
};

bool is_bound(const u64 bound, const QueryVar var) {
    return var.is_set() && (bound & (1ull << var.index)) != 0;
}

// The term's own archetype answers it: THIS source, nothing to bind, no
// traversal, not part of an or-chain. Optional ones are still plain, they
// just stay out of the matcher.
bool is_plain_this(const QueryTerm& term, const bool in_or) {
    return term.src_var == QUERY_THIS && !term.traverses() && !term.binds() && !in_or;
}

void report(const u32 term, const char* message) {
    fprintf(stderr, "[ecs] error: query term %u: %s\n", term, message);
}

bool validate_var(const QueryVar var, const usz var_count, const u32 term, const char* side) {
    if (!var.is_set()) {
        return true;
    }
    if (var.index >= var_count) {
        fprintf(stderr, "[ecs] error: query term %u: %s refers to variable %u, but the query has %llu\n",
            term, side, var.index, static_cast<unsigned long long>(var_count));
        return false;
    }
    return true;
}

// Checks what can be checked term by term. False with an error printed.
bool validate(World* world, const QueryTerm* terms, const usz term_count, const usz var_count) {
    if (var_count == 0 || var_count > QUERY_MAX_VARS) {
        fprintf(stderr, "[ecs] error: query has %llu variables; between 1 (THIS) and %llu are supported\n",
            static_cast<unsigned long long>(var_count), static_cast<unsigned long long>(QUERY_MAX_VARS));
        return false;
    }

    bool in_or = false;
    for (usz i = 0; i < term_count; i++) {
        const QueryTerm& term = terms[i];
        const u32 index = static_cast<u32>(i);

        if (term.id == 0) {
            report(index, "id is 0 (did a type fail to register?)");
            return false;
        }
        if (!validate_var(term.src_var, var_count, index, "source") || !validate_var(term.first_var, var_count, index, "first side")
            || !validate_var(term.second_var, var_count, index, "second side")) {
            return false;
        }
        if (!term.src_var.is_set() && term.src == 0) {
            report(index, "source is neither a variable nor an entity");
            return false;
        }
        if (term.first_var == QUERY_THIS || term.second_var == QUERY_THIS) {
            report(index, "THIS cannot be a side of a pair; it is only ever the source");
            return false;
        }
        if (term.first_var.is_set() && term.first_var == term.second_var) {
            report(index, "the same variable on both sides of a pair is not supported");
            return false;
        }
        if (term.is_excluded() && term.is_optional()) {
            report(index, "a without() term cannot be optional");
            return false;
        }
        if (term.traverses()) {
            if (!ENTITY::has(world, term.traverse, ECS::TRAVERSABLE)) {
                fprintf(stderr, "[ecs] error: query term %u: up(%llx) walks a relation that is not TRAVERSABLE\n",
                    index, static_cast<unsigned long long>(term.traverse));
                return false;
            }
            if (ECS::ENTITY_LOW(term.traverse) > (ECS::ID_COMPONENT_MASK >> ECS::ENTITY_BITS)) {
                report(index, "up() relation does not fit a pair's relation side");
                return false;
            }
        }

        const bool chained = in_or || term.is_or();
        if (chained && term.is_optional()) {
            report(index, "an bor() alternative cannot be optional");
            return false;
        }
        if (term.is_or() && i + 1 == term_count) {
            report(index, "bor() on the last term: nothing follows it");
            return false;
        }
        in_or = term.is_or();
    }
    return true;
}

// Whether every variable the unit needs bound is bound: its source, and
// its sides when the term cannot bind them itself (excluded, or part of an
// or-chain).
bool is_ready(const QueryTerm* terms, const Unit& unit, const u64 bound) {
    for (u32 i = unit.term; i < unit.term + unit.term_count; i++) {
        const QueryTerm& term = terms[i];
        if (term.src_var.is_set() && !is_bound(bound, term.src_var)) {
            return false;
        }
        const bool must_be_bound = term.is_excluded() || unit.is_or;
        if (must_be_bound) {
            if (term.first_var.is_set() && !is_bound(bound, term.first_var)) {
                return false;
            }
            if (term.second_var.is_set() && !is_bound(bound, term.second_var)) {
                return false;
            }
        }
    }
    return true;
}

QueryOp make_op(const QueryTerm& term, const u32 index, u64& bound) {
    QueryOp op;
    op.term = index;
    op.term_count = 1;
    op.optional = term.is_optional();
    if (term.traverses()) {
        op.kind = QUERY_OP_UP;
    } else if (term.is_excluded()) {
        op.kind = QUERY_OP_NOT;
    } else {
        op.kind = QUERY_OP_AND;
    }
    if (term.first_var.is_set() && !is_bound(bound, term.first_var)) {
        op.bind_first = term.first_var.index;
        bound |= 1ull << term.first_var.index;
    }
    if (term.second_var.is_set() && !is_bound(bound, term.second_var)) {
        op.bind_second = term.second_var.index;
        bound |= 1ull << term.second_var.index;
    }
    return op;
}

} // namespace

QueryProgram compile(World* world, const QueryTerm* terms, const usz term_count, const usz var_count, BaseAllocator* allocator) {
    QueryProgram program;
    program.allocator = allocator;
    program.ok = false;

    // Copy the terms and number the fields first, so a rejected program is
    // still a complete object to free.
    program.terms = allocator->allocate_array<QueryTerm>(term_count + 1);
    program.term_fields = allocator->allocate_array<u32>(term_count + 1);
    program.term_count = term_count;
    program.var_count = var_count;
    for (usz i = 0; i < term_count; i++) {
        program.terms[i] = terms[i];
        if (terms[i].is_output()) {
            program.term_fields[i] = static_cast<u32>(program.field_count);
            program.field_count++;
        } else {
            program.term_fields[i] = QUERY_OP_NONE;
        }
    }
    program.with_ids = allocator->allocate_array<Id>(term_count + 1);
    program.without_ids = allocator->allocate_array<Id>(term_count + 1);
    program.this_terms = allocator->allocate_array<u32>(term_count + 1);
    program.ops = allocator->allocate_array<QueryOp>(term_count + 2);
    program.op_count = 0;
    program.matcher = ArchetypeMatcher::create(allocator, nullptr, 0, nullptr, 0);

    if (!validate(world, terms, term_count, var_count)) {
        return program;
    }

    // --- Split: matcher / plain THIS terms / units for the VM ---------------
    Unit* units = allocator->allocate_array<Unit>(term_count + 1);
    usz unit_count = 0;

    usz i = 0;
    while (i < term_count) {
        const QueryTerm& term = terms[i];
        const u32 index = static_cast<u32>(i);

        if (term.is_or()) {
            // The whole chain is one unit; it contributes nothing to the matcher.
            Unit unit;
            unit.term = index;
            unit.is_or = true;
            usz j = i;
            while (j < term_count && terms[j].is_or()) {
                if (terms[j].src_var == QUERY_THIS) {
                    program.binds_this = true;
                }
                j++;
            }
            if (terms[j].src_var == QUERY_THIS) {
                program.binds_this = true;
            }
            unit.term_count = static_cast<u32>(j + 1 - i);
            units[unit_count++] = unit;
            i = j + 1;
            continue;
        }

        if (term.src_var == QUERY_THIS) {
            program.binds_this = true;
        }

        if (is_plain_this(term, false)) {
            program.this_terms[program.this_term_count++] = index;
            if (!term.is_optional()) {
                if (term.is_excluded()) {
                    program.without_ids[program.without_count++] = term.id;
                } else {
                    program.with_ids[program.with_count++] = term.id;
                }
            }
            i++;
            continue;
        }

        // The VM evaluates it. Still tell the matcher what THIS must hold
        // for the term to have any chance.
        if (term.src_var == QUERY_THIS && !term.is_optional() && !term.is_excluded()) {
            if (term.traverses()) {
                program.with_ids[program.with_count++] = ECS::PAIR(term.traverse, ECS::WILDCARD);
            } else {
                program.with_ids[program.with_count++] = term.id;
            }
        }
        Unit unit;
        unit.term = index;
        units[unit_count++] = unit;
        i++;
    }

    program.matcher.free();
    program.matcher = ArchetypeMatcher::create(allocator, program.with_ids, program.with_count, program.without_ids, program.without_count);

    // --- Schedule the ops ----------------------------------------------------
    u64 bound = 0;
    if (program.binds_this) {
        QueryOp select;
        select.kind = QUERY_OP_SELECT;
        program.ops[program.op_count++] = select;
        bound |= 1ull << QUERY_THIS.index;
    }

    // Each pass emits the first unit whose variables are bound and starts
    // over, so a term that had to wait comes right after the term that
    // binds what it needs, and the given order is otherwise kept.
    usz pending = unit_count;
    while (pending > 0) {
        bool progress = false;
        for (usz u = 0; u < unit_count; u++) {
            Unit& unit = units[u];
            if (unit.done || !is_ready(terms, unit, bound)) {
                continue;
            }
            if (unit.is_or) {
                QueryOp op;
                op.kind = QUERY_OP_OR;
                op.term = unit.term;
                op.term_count = unit.term_count;
                program.ops[program.op_count++] = op;
            } else {
                program.ops[program.op_count++] = make_op(terms[unit.term], unit.term, bound);
            }
            unit.done = true;
            pending--;
            progress = true;
            break;
        }
        if (!progress) {
            for (usz u = 0; u < unit_count; u++) {
                if (!units[u].done) {
                    report(units[u].term, "uses a variable no term binds before it (a source, or a side of a without() / bor() term)");
                    break;
                }
            }
            allocator->free(units);
            return program;
        }
    }
    allocator->free(units);

    QueryOp yield;
    yield.kind = QUERY_OP_YIELD;
    program.ops[program.op_count++] = yield;

    program.ok = true;
    return program;
}

const char* op_name(const QueryOpKind kind) {
    switch (kind) {
        case QUERY_OP_SELECT: return "select";
        case QUERY_OP_AND: return "and";
        case QUERY_OP_NOT: return "not";
        case QUERY_OP_UP: return "up";
        case QUERY_OP_OR: return "or";
        case QUERY_OP_YIELD: return "yield";
    }
    return "?";
}

} // namespace QUERY_PROGRAM

void QueryProgram::free() {
    if (this->allocator == nullptr) {
        return;
    }
    this->matcher.free();
    this->allocator->free(this->terms);
    this->allocator->free(this->term_fields);
    this->allocator->free(this->with_ids);
    this->allocator->free(this->without_ids);
    this->allocator->free(this->this_terms);
    this->allocator->free(this->ops);
    this->terms = nullptr;
    this->term_fields = nullptr;
    this->with_ids = nullptr;
    this->without_ids = nullptr;
    this->this_terms = nullptr;
    this->ops = nullptr;
    this->term_count = 0;
    this->field_count = 0;
    this->var_count = 0;
    this->with_count = 0;
    this->without_count = 0;
    this->this_term_count = 0;
    this->op_count = 0;
    this->binds_this = false;
    this->ok = false;
}
