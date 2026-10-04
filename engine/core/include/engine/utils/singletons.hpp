#pragma once

#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/hash_map.hpp"
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
// Storage is lazy and there is no destructor: call free() when done. Values
// are destroyed through their destructor when removed or freed, which for
// engine types (no destructors) means nothing: anything a singleton owns must
// be released explicitly before remove() / free(), like any other container.
struct Singletons {
    struct Entry {
        void* value = nullptr;
        // Runs T's destructor on `value`; the store then frees the memory.
        void (*destroy)(void* value) = nullptr;
    };

    BaseAllocator* allocator = nullptr;
    HashMap<TypeId, Entry> entries;

    Singletons();
    explicit Singletons(BaseAllocator* allocator);

    Singletons(const Singletons&) = delete;
    Singletons& operator=(const Singletons&) = delete;

    // Creates T's singleton as T(args...) and returns it. If T already has a
    // singleton an error is printed, nothing changes and nullptr is returned:
    // remove<T>() it first to replace it.
    template <typename T, typename... Args>
    T* create(Args&&... args);

    // The singleton for T, or nullptr if create<T>() has not been called (or
    // the value was removed since). Never creates.
    template <typename T>
    T* get() const;

    // Whether T's singleton exists.
    template <typename T>
    bool has() const;

    // Destroys T's singleton and releases its memory. False if there is none.
    template <typename T>
    bool remove();

    // Number of singletons currently stored.
    usz count() const { return this->entries.count; }

    // Destroys every singleton and releases the storage, returning the store
    // to its freshly constructed state.
    void free();

private:
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
        fprintf(stderr, "[singletons] error: singleton for type id %llu already exists; remove it before creating it again\n", type_id);
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

    const Entry* entry = this->entries.find(TYPE_ID::get<T>());
    return entry != nullptr ? static_cast<T*>(entry->value) : nullptr;
}

template <typename T>
bool Singletons::has() const {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "Singletons::has<T>: pass the plain type, without const / volatile / reference");

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
