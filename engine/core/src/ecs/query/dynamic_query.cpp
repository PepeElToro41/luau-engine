#include "engine/ecs/query/dynamic_query.hpp"

#include "engine/ecs/query/query_builder.hpp"
#include "engine/ecs/query/query_scan.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

// --- Build -------------------------------------------------------------------

DynamicQuery QueryBuilder::build() {
    DynamicQuery query;
    query.world = this->world;
    query.flags = this->flags;
    query.program = QUERY_PROGRAM::compile(this->world, this->terms.data, this->terms.count, this->var_count(), this->world->allocator);
    query.var_names = std::move(this->var_names);
    this->free();
    return query;
}

// --- Lifecycle ---------------------------------------------------------------

DynamicQuery::DynamicQuery(DynamicQuery&& other) noexcept :
    world(other.world),
    flags(other.flags),
    program(other.program),
    cache(other.cache),
    var_names(std::move(other.var_names))
{
    other.program = QueryProgram { };
    other.cache = nullptr;
    other.world = nullptr;
}

DynamicQuery& DynamicQuery::operator=(DynamicQuery&& other) noexcept {
    if (this != &other) {
        this->free();
        this->world = other.world;
        this->flags = other.flags;
        this->program = other.program;
        this->cache = other.cache;
        this->var_names = std::move(other.var_names);
        other.program = QueryProgram { };
        other.cache = nullptr;
        other.world = nullptr;
    }
    return *this;
}

void DynamicQuery::free() {
    this->cleanup();
    this->program.free();
    if (this->world != nullptr) {
        for (char* name : this->var_names) {
            this->world->allocator->free(name);
        }
    }
    this->var_names.free();
}

// --- Cache -------------------------------------------------------------------

void DynamicQuery::cleanup() {
    QUERY_SCAN::destroy_cache(this->cache);
    this->cache = nullptr;
}

void DynamicQuery::ensure_cache() {
    if ((this->flags & QUERY_CACHED) == 0 || this->cache != nullptr || !this->program.ok || !this->program.binds_this) {
        return;
    }
    // The cache keeps a column per plain THIS term, in this_terms order,
    // which is how the VM reads them back.
    TemporalAllocator temp = TemporalAllocator::create();
    QueryTerm* terms = temp.allocate_array<QueryTerm>(this->program.this_term_count + 1);
    for (usz i = 0; i < this->program.this_term_count; i++) {
        terms[i] = this->program.terms[this->program.this_terms[i]];
    }
    this->cache = QUERY_SCAN::create_cache(this->world, this->program.with_ids, this->program.with_count,
        this->program.without_ids, this->program.without_count, terms, this->program.this_term_count, this->program.allocator);
}

// --- Variables ---------------------------------------------------------------

QueryVar DynamicQuery::var(const char* name) const {
    if (name == nullptr || name[0] == '\0') {
        return QueryVar { };
    }
    if (std::strcmp(name, "this") == 0) {
        return QUERY_THIS;
    }
    for (usz i = 0; i < this->var_names.count; i++) {
        if (std::strcmp(this->var_names.data[i], name) == 0) {
            return QueryVar { static_cast<u32>(i + 1) };
        }
    }
    return QueryVar { };
}

const char* DynamicQuery::var_name(const QueryVar variable) const {
    if (variable == QUERY_THIS) {
        return "this";
    }
    if (!variable.is_set() || variable.index > this->var_names.count) {
        return nullptr;
    }
    return this->var_names.data[variable.index - 1];
}

// --- Iteration ---------------------------------------------------------------

bool DynamicQuery::check_fields(const usz type_count, const char* what) const {
    if (type_count == this->program.field_count) {
        return true;
    }
    fprintf(stderr, "[ecs] error: %s() given %llu output types, but the query has %llu output terms\n",
        what, static_cast<unsigned long long>(type_count), static_cast<unsigned long long>(this->program.field_count));
    return false;
}

QueryIter DynamicQuery::begin(BaseAllocator* allocator) {
    this->ensure_cache();
    return QUERY_VM::begin(this->world, &this->program, this->cache, allocator);
}

usz DynamicQuery::count() {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    return QUERY::count(it);
}

bool DynamicQuery::empty() {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    return QUERY::empty(it);
}

EntityId DynamicQuery::first() {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    return QUERY::first(it);
}

EntityId DynamicQuery::random(u64& rng_state) {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    return QUERY::random(it, rng_state);
}

bool DynamicQuery::matches(const EntityId entity) {
    return QUERY_VM::matches(this->world, &this->program, entity);
}
