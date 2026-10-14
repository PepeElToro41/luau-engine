#pragma once

#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/hash_map.hpp"
#include "engine/utils/log.hpp"
#include "engine/utils/type_id.hpp"

#include <cstdio>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// One value per C++ type: a typed service locator for globals such as the
// world, input state or asset caches. Values are created explicitly with
// create<T>(args...) and looked up with get<T>().
//
//     Singletons singletons;
//     singletons.create<Clock>(60.0);       // constructs Clock{60.0}
//     singletons.get<Clock>()->time += dt;  // nullptr if never created
//     singletons.free();
//
// Each value lives in its own allocation on the store's allocator, so the
// pointer create() / get() hand out stays valid until that value is removed
// or the store is freed, however many other singletons are added later. The
// key is TYPE_ID::get<T>(), so `T`, `const T` and `T&` would be different
// singletons: always pass the plain type (the templates enforce it).
//
// Stores form a hierarchy: a store with a `parent` resolves a type it has
// no entry for through the parent, and so on up. That is how a play
// instance in the editor holds its own World, Scene and renderer while
// everything shared (the asset provider, the project, the GPU caches) is
// found in the root store, and how every system keeps asking one store
// without knowing which instance it serves. Whether a type may be resolved
// upward is declared once per type, process-wide:
//
//     SINGLETON_SCOPE::declare<World>(SCOPE_LOCAL, "World");        // each instance has its own
//     SINGLETON_SCOPE::declare<AssetResourceProvider>(SCOPE_SHARED, "AssetResourceProvider");
//
// A child's get<T>() for a SCOPE_LOCAL type never walks up: it logs and
// returns nullptr rather than hand back the parent's value, so an instance
// that forgot to create its World cannot silently run against the
// authoring one. A type with no declaration is treated as local, with the
// same refusal and a one-time warning, since sharing by accident is the
// dangerous mistake and the fix is one declaration. check_locals() finds
// the omissions at creation rather than on first use. Only lookups go
// through the parent: has_local(), remove() and free() are this store's
// alone, so a child never touches what its parent owns.
//
// Storage is lazy and there is no destructor: call free() when done. Values
// are destroyed through their destructor when removed or freed, which for
// engine types (no destructors) means nothing: anything a singleton owns must
// be released explicitly before remove() / free(), like any other container.

enum SingletonScope : u32 {
    // Never declared: resolved like SCOPE_LOCAL, with a warning once.
    SCOPE_UNDECLARED = 0,
    // One per process: a child store resolves it through its parent.
    SCOPE_SHARED = 1,
    // One per instance: a child must hold its own, never its parent's.
    SCOPE_LOCAL = 2,
};

namespace SINGLETON_SCOPE {

// Declared types the table holds; a fixed table, no allocation, usable
// before main.
constexpr usz MAX_TYPES = 256;

struct Entry {
    TypeId type = 0;
    SingletonScope scope = SCOPE_UNDECLARED;
    // A string literal, never copied; nullptr when none was given.
    const char* name = nullptr;
};

// Declares the scope of `type`. Declaring the same scope again is a no-op;
// another scope for a type already declared is an error and keeps the
// first. False (with an error) when refused or the table is full.
bool declare(TypeId type, SingletonScope scope, const char* name = nullptr);
template <typename T>
bool declare(const SingletonScope scope, const char* name = nullptr) {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "SINGLETON_SCOPE::declare<T>: pass the plain type, without const / volatile / reference");
    return SINGLETON_SCOPE::declare(TYPE_ID::get<T>(), scope, name);
}

// The declared scope, SCOPE_UNDECLARED when none.
SingletonScope of(TypeId type);
template <typename T>
SingletonScope of() {
    return SINGLETON_SCOPE::of(TYPE_ID::get<T>());
}
// The name given at declaration, or nullptr.
const char* name_of(TypeId type);

usz count();
const Entry& entry(usz index);

// Forgets every declaration (and every one-time warning). For tests.
void reset();

} // namespace SINGLETON_SCOPE

struct Singletons {
    struct Entry {
        void* value = nullptr;
        // Runs T's destructor on `value`; the store then frees the memory.
        void (*destroy)(void* value) = nullptr;
    };

    BaseAllocator* allocator = nullptr;
    HashMap<TypeId, Entry> entries;
    // Where a type this store lacks is looked for next, when its scope
    // allows; nullptr for a root store. Set by whoever makes the child.
    Singletons* parent = nullptr;

    Singletons();
    explicit Singletons(BaseAllocator* allocator);

    Singletons(const Singletons&) = delete;
    Singletons& operator=(const Singletons&) = delete;

    // Creates T's singleton as T(args...) in this store and returns it. If T
    // already has a singleton here an error is printed, nothing changes and
    // nullptr is returned: remove<T>() it first to replace it. A value the
    // parent holds is shadowed, not replaced.
    template <typename T, typename... Args>
    T* create(Args&&... args);

    // The singleton for T: this store's, else the parent chain's when T is
    // SCOPE_SHARED, else nullptr (with a log for a local type a child
    // lacks). Never creates.
    template <typename T>
    T* get() const;

    // Whether get<T>() would find a value.
    template <typename T>
    bool has() const;
    // Whether this store itself holds T.
    template <typename T>
    bool has_local() const;

    // Destroys this store's singleton for T and releases its memory. False
    // if this store has none (a parent's is never touched).
    template <typename T>
    bool remove();

    // Number of singletons stored here.
    usz count() const { return this->entries.count; }

    // Whether every SCOPE_LOCAL type some ancestor holds is also held here,
    // each omission logged: what a new instance checks before running.
    // True for a root store.
    bool check_locals() const;

    // Destroys every singleton of this store and releases the storage,
    // returning the store to its freshly constructed state. The parent is
    // left as it was, and so is the link to it.
    void free();

private:
    // The raw lookup by type id, walking up when the scope allows; `report`
    // logs a local type a child lacks (get), has() asks quietly.
    void* resolve(TypeId type_id, bool report) const;
    static void warn_undeclared(TypeId type_id);

    template <typename T>
    static void destroy_value(void* value) {
        std::destroy_at(static_cast<T*>(value));
    }
};

template <typename T, typename... Args>
T* Singletons::create(Args&&... args) {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "Singletons::create<T>: pass the plain type, without const / volatile / reference");
    static_assert(std::is_constructible_v<T, Args&&...>, "Singletons::create<T>: T cannot be constructed from these arguments");

    const TypeId type_id = TYPE_ID::get<T>();
    if (this->entries.contains(type_id)) {
        const char* name = SINGLETON_SCOPE::name_of(type_id);
        LOG::error(LOG_CATEGORY::ENGINE, "singleton %s%s(type id %llu) already exists; remove it before creating it again", name != nullptr ? name : "",
            name != nullptr ? " " : "", static_cast<unsigned long long>(type_id));
        return nullptr;
    }

    T* value = this->allocator->allocate_array<T>(1);
    new (value) T(std::forward<Args>(args)...);

    Entry entry;
    entry.value = value;
    entry.destroy = &Singletons::destroy_value<T>;
    this->entries.insert(type_id, entry);
    return value;
}

template <typename T>
T* Singletons::get() const {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "Singletons::get<T>: pass the plain type, without const / volatile / reference");

    return static_cast<T*>(this->resolve(TYPE_ID::get<T>(), true));
}

template <typename T>
bool Singletons::has() const {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "Singletons::has<T>: pass the plain type, without const / volatile / reference");

    return this->resolve(TYPE_ID::get<T>(), false) != nullptr;
}

template <typename T>
bool Singletons::has_local() const {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "Singletons::has_local<T>: pass the plain type, without const / volatile / reference");

    return this->entries.contains(TYPE_ID::get<T>());
}

template <typename T>
bool Singletons::remove() {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "Singletons::remove<T>: pass the plain type, without const / volatile / reference");

    const TypeId type_id = TYPE_ID::get<T>();
    const Entry* entry = this->entries.find(type_id);
    if (entry == nullptr) {
        return false;
    }
    entry->destroy(entry->value);
    this->allocator->free(entry->value);
    this->entries.remove(type_id);
    return true;
}
