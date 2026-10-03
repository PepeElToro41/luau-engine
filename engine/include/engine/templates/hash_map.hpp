#pragma once

#include "engine/memory/base_allocator.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <utility>

// Open-addressing hash map with linear probing, a replacement for
// std::unordered_map. Capacity is always a power of two; slots are marked
// empty, occupied, or tombstone (deleted) in a side table so lookups can keep
// probing past removed keys.
//
// Storage is lazy: a fresh map owns nothing (entries == nullptr, capacity == 0)
// and the first insert allocates. There is no destructor; call free() when the
// map is no longer needed. Copying is disabled; moving transfers the storage
// and leaves the source empty.
template <typename K, typename V, typename Hash = std::hash<K>, typename Equal = std::equal_to<K>>
struct HashMap {
    struct Entry {
        K key;
        V value;
    };

    HashMap() : allocator(MEMORY::heap_allocator()) {}
    explicit HashMap(BaseAllocator* allocator) : allocator(allocator) {}

    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;

    HashMap(HashMap&& other) noexcept
        : allocator(other.allocator), entries(other.entries), states(other.states),
          count(other.count), capacity(other.capacity), tombstones(other.tombstones) {
        other.entries = nullptr;
        other.states = nullptr;
        other.count = 0;
        other.capacity = 0;
        other.tombstones = 0;
    }

    HashMap& operator=(HashMap&& other) noexcept {
        if (this != &other) {
            this->free();
            this->allocator = other.allocator;
            this->entries = other.entries;
            this->states = other.states;
            this->count = other.count;
            this->capacity = other.capacity;
            this->tombstones = other.tombstones;
            other.entries = nullptr;
            other.states = nullptr;
            other.count = 0;
            other.capacity = 0;
            other.tombstones = 0;
        }
        return *this;
    }

    // --- Lookup -------------------------------------------------------------

    V* find(const K& key) {
        size_t slot = this->find_slot(key);
        return slot == npos ? nullptr : &this->entries[slot].value;
    }

    const V* find(const K& key) const {
        size_t slot = this->find_slot(key);
        return slot == npos ? nullptr : &this->entries[slot].value;
    }

    bool contains(const K& key) const {
        return this->find_slot(key) != npos;
    }

    // Returns the value for `key`, default-constructing it if absent.
    V& operator[](const K& key) {
        size_t slot = this->find_slot(key);
        if (slot != npos) {
            return this->entries[slot].value;
        }
        slot = this->claim_slot(key);
        new (&this->entries[slot].key) K(key);
        new (&this->entries[slot].value) V();
        return this->entries[slot].value;
    }

    // --- Modifiers ----------------------------------------------------------

    // Inserts `value` under `key`, overwriting any existing value. Returns a
    // reference to the stored value.
    template <typename U>
    V& insert(const K& key, U&& value) {
        size_t slot = this->find_slot(key);
        if (slot != npos) {
            this->entries[slot].value = std::forward<U>(value);
            return this->entries[slot].value;
        }
        slot = this->claim_slot(key);
        new (&this->entries[slot].key) K(key);
        new (&this->entries[slot].value) V(std::forward<U>(value));
        return this->entries[slot].value;
    }

    // Removes `key` if present. Returns whether anything was removed.
    bool remove(const K& key) {
        size_t slot = this->find_slot(key);
        if (slot == npos) {
            return false;
        }
        std::destroy_at(&this->entries[slot]);
        this->states[slot] = slot_tombstone;
        this->count -= 1;
        this->tombstones += 1;
        return true;
    }

    // Makes room for at least `expected_count` entries without rehashing.
    void reserve(size_t expected_count) {
        size_t needed = capacity_for(expected_count);
        if (needed > this->capacity) {
            this->rehash(needed);
        }
    }

    // Destroys every entry but keeps the storage.
    void clear() {
        for (size_t i = 0; i < this->capacity; ++i) {
            if (this->states[i] == slot_occupied) {
                std::destroy_at(&this->entries[i]);
            }
        }
        if (this->capacity > 0) {
            std::memset(this->states, slot_empty, this->capacity);
        }
        this->count = 0;
        this->tombstones = 0;
    }

    // Destroys every entry and releases the storage, returning the map to its
    // freshly constructed state.
    void free() {
        this->clear();
        this->allocator->free(this->entries);
        this->entries = nullptr;
        this->states = nullptr;
        this->capacity = 0;
    }

    bool is_empty() const { return this->count == 0; }

    // --- Iteration ----------------------------------------------------------

    template <typename MapT, typename EntryT>
    struct BasicIterator {
        MapT* map;
        size_t index;

        EntryT& operator*() const { return this->map->entries[this->index]; }
        EntryT* operator->() const { return &this->map->entries[this->index]; }

        BasicIterator& operator++() {
            this->index += 1;
            this->skip_to_occupied();
            return *this;
        }

        bool operator==(const BasicIterator& other) const { return this->index == other.index; }
        bool operator!=(const BasicIterator& other) const { return this->index != other.index; }

        void skip_to_occupied() {
            while (this->index < this->map->capacity && this->map->states[this->index] != slot_occupied) {
                this->index += 1;
            }
        }
    };

    using Iterator = BasicIterator<HashMap, Entry>;
    using ConstIterator = BasicIterator<const HashMap, const Entry>;

    Iterator begin() {
        Iterator it{this, 0};
        it.skip_to_occupied();
        return it;
    }
    Iterator end() { return Iterator{this, this->capacity}; }

    ConstIterator begin() const {
        ConstIterator it{this, 0};
        it.skip_to_occupied();
        return it;
    }
    ConstIterator end() const { return ConstIterator{this, this->capacity}; }

    // --- Members ------------------------------------------------------------

    BaseAllocator* allocator = nullptr;
    Entry* entries = nullptr;
    uint8_t* states = nullptr;
    size_t count = 0;
    size_t capacity = 0;
    size_t tombstones = 0;

private:
    static constexpr size_t npos = static_cast<size_t>(-1);
    static constexpr size_t minimum_capacity = 8;

    static constexpr uint8_t slot_empty = 0;
    static constexpr uint8_t slot_occupied = 1;
    static constexpr uint8_t slot_tombstone = 2;

    // Entries and the state table live in one allocation: entries first, then
    // one byte per slot.
    static size_t storage_bytes(size_t capacity) {
        return capacity * sizeof(Entry) + capacity;
    }

    // Smallest power-of-two capacity that keeps `entry_count` entries under the
    // 75% load limit.
    static size_t capacity_for(size_t entry_count) {
        size_t needed = entry_count * 4 / 3 + 1;
        size_t result = minimum_capacity;
        while (result < needed) {
            result *= 2;
        }
        return result;
    }

    static size_t hash_key(const K& key) {
        // std::hash is often the identity for integers; a finalizer spreads the
        // bits so masking to a power of two does not cluster.
        uint64_t h = static_cast<uint64_t>(Hash{}(key));
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return static_cast<size_t>(h);
    }

    // Index of the occupied slot holding `key`, or npos.
    size_t find_slot(const K& key) const {
        if (this->capacity == 0) {
            return npos;
        }
        size_t mask = this->capacity - 1;
        size_t index = hash_key(key) & mask;
        while (true) {
            uint8_t state = this->states[index];
            if (state == slot_empty) {
                return npos;
            }
            if (state == slot_occupied && Equal{}(this->entries[index].key, key)) {
                return index;
            }
            index = (index + 1) & mask;
        }
    }

    // Grows if needed, then returns a free slot for `key`, marking it occupied.
    // The caller must construct the entry in it. `key` must not already be
    // present.
    size_t claim_slot(const K& key) {
        if ((this->count + this->tombstones + 1) * 4 > this->capacity * 3) {
            // Rehash in place if tombstones are what filled us up, otherwise grow.
            size_t needed = capacity_for(this->count + 1);
            this->rehash(needed > this->capacity ? needed : this->capacity);
        }

        size_t mask = this->capacity - 1;
        size_t index = hash_key(key) & mask;
        while (this->states[index] == slot_occupied) {
            index = (index + 1) & mask;
        }
        if (this->states[index] == slot_tombstone) {
            this->tombstones -= 1;
        }
        this->states[index] = slot_occupied;
        this->count += 1;
        return index;
    }

    void rehash(size_t new_capacity) {
        Entry* old_entries = this->entries;
        uint8_t* old_states = this->states;
        size_t old_capacity = this->capacity;

        void* memory = this->allocator->allocate(storage_bytes(new_capacity), alignof(Entry));
        this->entries = static_cast<Entry*>(memory);
        this->states = reinterpret_cast<uint8_t*>(this->entries + new_capacity);
        std::memset(this->states, slot_empty, new_capacity);
        this->capacity = new_capacity;
        this->tombstones = 0;

        size_t mask = new_capacity - 1;
        for (size_t i = 0; i < old_capacity; ++i) {
            if (old_states[i] != slot_occupied) {
                continue;
            }
            size_t index = hash_key(old_entries[i].key) & mask;
            while (this->states[index] == slot_occupied) {
                index = (index + 1) & mask;
            }
            new (&this->entries[index]) Entry(std::move(old_entries[i]));
            std::destroy_at(&old_entries[i]);
            this->states[index] = slot_occupied;
        }

        this->allocator->free(old_entries);
    }
};
