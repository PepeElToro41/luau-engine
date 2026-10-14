#include "engine/utils/singletons.hpp"

#include "engine/memory/heap_allocator.hpp"

// --- Scopes --------------------------------------------------------------------

namespace {

SINGLETON_SCOPE::Entry scope_entries[SINGLETON_SCOPE::MAX_TYPES];
usz scope_count = 0;

// Types a child was asked for without a declaration, each warned about once.
constexpr usz MAX_WARNED = 64;
TypeId warned[MAX_WARNED];
usz warned_count = 0;

SINGLETON_SCOPE::Entry* find_scope(const TypeId type) {
    for (usz i = 0; i < scope_count; ++i) {
        if (scope_entries[i].type == type) {
            return &scope_entries[i];
        }
    }
    return nullptr;
}

const char* scope_name(const SingletonScope scope) {
    switch (scope) {
    case SCOPE_SHARED:
        return "shared";
    case SCOPE_LOCAL:
        return "local";
    default:
        return "undeclared";
    }
}

} // namespace

bool SINGLETON_SCOPE::declare(const TypeId type, const SingletonScope scope, const char* name) {
    if (type == 0 || scope == SCOPE_UNDECLARED) {
        return false;
    }
    if (Entry* existing = find_scope(type)) {
        if (existing->scope != scope) {
            LOG::error(LOG_CATEGORY::ENGINE, "singleton %s (type id %llu) is declared %s and cannot become %s", existing->name != nullptr ? existing->name : "?",
                static_cast<unsigned long long>(type), scope_name(existing->scope), scope_name(scope));
            return false;
        }
        if (existing->name == nullptr) {
            existing->name = name;
        }
        return true;
    }
    if (scope_count >= MAX_TYPES) {
        LOG::error(LOG_CATEGORY::ENGINE, "singleton scopes: the table of %llu types is full; cannot declare type %llu", static_cast<unsigned long long>(MAX_TYPES),
            static_cast<unsigned long long>(type));
        return false;
    }
    Entry& entry = scope_entries[scope_count++];
    entry.type = type;
    entry.scope = scope;
    entry.name = name;
    return true;
}

SingletonScope SINGLETON_SCOPE::of(const TypeId type) {
    const Entry* entry = find_scope(type);
    return entry != nullptr ? entry->scope : SCOPE_UNDECLARED;
}

const char* SINGLETON_SCOPE::name_of(const TypeId type) {
    const Entry* entry = find_scope(type);
    return entry != nullptr ? entry->name : nullptr;
}

usz SINGLETON_SCOPE::count() {
    return scope_count;
}

const SINGLETON_SCOPE::Entry& SINGLETON_SCOPE::entry(const usz index) {
    return scope_entries[index];
}

void SINGLETON_SCOPE::reset() {
    for (usz i = 0; i < scope_count; ++i) {
        scope_entries[i] = Entry {};
    }
    scope_count = 0;
    warned_count = 0;
}

// --- Store ---------------------------------------------------------------------

Singletons::Singletons(BaseAllocator* allocator) :
    allocator(allocator),
    entries(allocator)
{}

Singletons::Singletons() : Singletons(MEMORY::heap_allocator()) {}

void Singletons::warn_undeclared(const TypeId type_id) {
    for (usz i = 0; i < warned_count; ++i) {
        if (warned[i] == type_id) {
            return;
        }
    }
    if (warned_count < MAX_WARNED) {
        warned[warned_count++] = type_id;
    }
    LOG::warning(LOG_CATEGORY::ENGINE, "singleton type id %llu has no declared scope; a child store will not resolve it through its parent (declare it with SINGLETON_SCOPE::declare<T>)",
        static_cast<unsigned long long>(type_id));
}

void* Singletons::resolve(const TypeId type_id, const bool report) const {
    const Entry* entry = this->entries.find(type_id);
    if (entry != nullptr) {
        return entry->value;
    }
    if (this->parent == nullptr) {
        return nullptr;
    }
    switch (SINGLETON_SCOPE::of(type_id)) {
    case SCOPE_SHARED:
        return this->parent->resolve(type_id, report);
    case SCOPE_LOCAL:
        // Refused rather than silently served the parent's: an instance
        // without its own copy of a local type is a bug to see at once.
        if (report && this->parent->resolve(type_id, false) != nullptr) {
            const char* name = SINGLETON_SCOPE::name_of(type_id);
            LOG::error(LOG_CATEGORY::ENGINE, "singleton %s%s(type id %llu) is local to an instance and this one has none; the parent's is not handed out",
                name != nullptr ? name : "", name != nullptr ? " " : "", static_cast<unsigned long long>(type_id));
        }
        return nullptr;
    default:
        if (report) {
            Singletons::warn_undeclared(type_id);
        }
        return nullptr;
    }
}

bool Singletons::check_locals() const {
    bool ok = true;
    for (const Singletons* ancestor = this->parent; ancestor != nullptr; ancestor = ancestor->parent) {
        for (const auto& entry : ancestor->entries) {
            if (SINGLETON_SCOPE::of(entry.key) != SCOPE_LOCAL || this->entries.contains(entry.key)) {
                continue;
            }
            const char* name = SINGLETON_SCOPE::name_of(entry.key);
            LOG::error(LOG_CATEGORY::ENGINE, "instance is missing its own %s%s(type id %llu), a local singleton its parent holds", name != nullptr ? name : "",
                name != nullptr ? " " : "", static_cast<unsigned long long>(entry.key));
            ok = false;
        }
    }
    return ok;
}

void Singletons::free() {
    for (auto& entry : this->entries) {
        entry.value.destroy(entry.value.value);
        this->allocator->free(entry.value.value);
    }
    this->entries.free();
}
