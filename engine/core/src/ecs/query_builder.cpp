#include "engine/ecs/query_builder.hpp"

#include <cstdio>
#include <cstring>

QueryBuilder::QueryBuilder(World* world, const u32 flags)
    : world(world), flags(flags), terms(world->allocator), var_names(world->allocator) {}

QueryBuilder World::query_build(const u32 flags) {
    return QueryBuilder(this, flags);
}

void QueryBuilder::free() {
    for (char* name : this->var_names) {
        this->world->allocator->free(name);
    }
    this->var_names.free();
    this->terms.free();
}

// --- Terms -------------------------------------------------------------------

QueryBuilder& QueryBuilder::add(const Id id, const QueryVar first_var, const QueryVar second_var, const u32 term_flags) {
    QueryTerm term = QueryTerm::make(id, term_flags);
    term.first_var = first_var;
    term.second_var = second_var;
    this->terms.push(term);
    return *this;
}

QueryBuilder& QueryBuilder::add_pair_second_var(const Id first, const QueryVar second, const u32 term_flags) {
    if (first == 0) {
        return this->add(0, QueryVar {}, second, term_flags);
    }
    return this->add(ECS::PAIR(first, ECS::WILDCARD), QueryVar {}, second, term_flags);
}

QueryBuilder& QueryBuilder::add_pair_first_var(const QueryVar first, const Id second, const u32 term_flags) {
    if (second == 0) {
        return this->add(0, first, QueryVar {}, term_flags);
    }
    return this->add(ECS::PAIR(ECS::WILDCARD, second), first, QueryVar {}, term_flags);
}

QueryBuilder& QueryBuilder::term(const Id id) {
    return this->add(id, QueryVar {}, QueryVar {}, TERM_OUTPUT);
}

QueryBuilder& QueryBuilder::term(const Id first, const QueryVar second) {
    return this->add_pair_second_var(first, second, TERM_OUTPUT);
}

QueryBuilder& QueryBuilder::term(const QueryVar first, const Id second) {
    return this->add_pair_first_var(first, second, TERM_OUTPUT);
}

QueryBuilder& QueryBuilder::with(const Id first, const QueryVar second) {
    return this->add_pair_second_var(first, second, 0);
}

QueryBuilder& QueryBuilder::with(const QueryVar first, const Id second) {
    return this->add_pair_first_var(first, second, 0);
}

QueryBuilder& QueryBuilder::without(const Id first, const QueryVar second) {
    return this->add_pair_second_var(first, second, TERM_EXCLUDE);
}

QueryBuilder& QueryBuilder::without(const QueryVar first, const Id second) {
    return this->add_pair_first_var(first, second, TERM_EXCLUDE);
}

// --- Modifiers ---------------------------------------------------------------

QueryTerm* QueryBuilder::last(const char* modifier) {
    if (this->terms.count == 0) {
        fprintf(stderr, "[ecs] error: query_build().%s() called before any term was added; nothing to modify\n", modifier);
        return nullptr;
    }
    return &this->terms.last();
}

QueryBuilder& QueryBuilder::optional() {
    if (QueryTerm* term = this->last("optional")) {
        term->flags |= TERM_OPTIONAL;
    }
    return *this;
}

QueryBuilder& QueryBuilder::bor() {
    if (QueryTerm* term = this->last("bor")) {
        term->flags |= TERM_OR;
    }
    return *this;
}

QueryBuilder& QueryBuilder::src(const EntityId source) {
    if (QueryTerm* term = this->last("src")) {
        term->src = source;
        term->src_var = QueryVar {};
    }
    return *this;
}

QueryBuilder& QueryBuilder::src(const QueryVar source) {
    if (QueryTerm* term = this->last("src")) {
        term->src = 0;
        term->src_var = source;
    }
    return *this;
}

QueryBuilder& QueryBuilder::up(const Id relation) {
    if (QueryTerm* term = this->last("up")) {
        term->traverse = relation;
        term->flags |= TERM_UP;
    }
    return *this;
}

// --- Variables ---------------------------------------------------------------

QueryVar QueryBuilder::var(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return QueryVar {};
    }
    if (std::strcmp(name, "this") == 0) {
        return QUERY_THIS;
    }
    for (usz i = 0; i < this->var_names.count; i++) {
        if (std::strcmp(this->var_names.data[i], name) == 0) {
            return QueryVar { static_cast<u32>(i + 1) };
        }
    }

    const usz length = std::strlen(name);
    char* copy = this->world->allocator->allocate_array<char>(length + 1);
    std::memcpy(copy, name, length + 1);
    this->var_names.push(copy);
    return QueryVar { static_cast<u32>(this->var_names.count) };
}

const char* QueryBuilder::var_name(const QueryVar variable) const {
    if (variable == QUERY_THIS) {
        return "this";
    }
    if (!variable.is_set() || variable.index > this->var_names.count) {
        return nullptr;
    }
    return this->var_names.data[variable.index - 1];
}

usz QueryBuilder::field_count() const {
    usz count = 0;
    for (const QueryTerm& term : this->terms) {
        if (term.is_output()) {
            count++;
        }
    }
    return count;
}
