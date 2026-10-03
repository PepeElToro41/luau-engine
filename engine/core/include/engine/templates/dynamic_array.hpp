#pragma once

#include "engine/memory/base_allocator.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Growable contiguous array, a replacement for std::vector.
//
// Storage is lazy: a fresh array owns nothing (data == nullptr, capacity == 0)
// and the first push allocates. There is no destructor; call free() when the
// array is no longer needed. Copying is disabled so two arrays never share a
// buffer by accident; moving transfers the buffer and leaves the source empty.
template <typename T>
struct DynamicArray {
    DynamicArray() : allocator(MEMORY::heap_allocator()) {}
    explicit DynamicArray(BaseAllocator* allocator) : allocator(allocator) {}

    DynamicArray(const DynamicArray&) = delete;
    DynamicArray& operator=(const DynamicArray&) = delete;

    DynamicArray(DynamicArray&& other) noexcept
        : allocator(other.allocator), data(other.data), count(other.count), capacity(other.capacity) {
        other.data = nullptr;
        other.count = 0;
        other.capacity = 0;
    }

    DynamicArray& operator=(DynamicArray&& other) noexcept {
        if (this != &other) {
            this->free();
            this->allocator = other.allocator;
            this->data = other.data;
            this->count = other.count;
            this->capacity = other.capacity;
            other.data = nullptr;
            other.count = 0;
            other.capacity = 0;
        }
        return *this;
    }

    // --- Capacity -----------------------------------------------------------

    // Grows the buffer so it can hold at least `new_capacity` elements. Never
    // shrinks.
    void reserve(size_t new_capacity) {
        if (new_capacity <= this->capacity) {
            return;
        }

        // Always allocate + move + free rather than reallocate(): arenas
        // cannot resize a block, and this keeps the array usable on any allocator.
        T* fresh = this->allocator->allocate_array<T>(new_capacity);
        if constexpr (std::is_trivially_copyable_v<T>) {
            if (this->count > 0) {
                std::memcpy(fresh, this->data, this->count * sizeof(T));
            }
        } else {
            for (size_t i = 0; i < this->count; ++i) {
                new (fresh + i) T(std::move(this->data[i]));
                std::destroy_at(this->data + i);
            }
        }
        this->allocator->free(this->data);
        this->data = fresh;
        this->capacity = new_capacity;
    }

    // Sets the element count, default-constructing new elements or destroying
    // trailing ones.
    void resize(size_t new_count) {
        if (new_count > this->count) {
            this->reserve(new_count);
            for (size_t i = this->count; i < new_count; ++i) {
                new (this->data + i) T();
            }
        } else {
            std::destroy(this->data + new_count, this->data + this->count);
        }
        this->count = new_count;
    }

    // Destroys every element but keeps the buffer.
    void clear() {
        std::destroy(this->data, this->data + this->count);
        this->count = 0;
    }

    // Destroys every element and releases the buffer, returning the array to
    // its freshly constructed state.
    void free() {
        this->clear();
        this->allocator->free(this->data);
        this->data = nullptr;
        this->capacity = 0;
    }

    // --- Modifiers ----------------------------------------------------------

    T& push(const T& value) {
        this->grow_for_one_more();
        T* slot = new (this->data + this->count) T(value);
        this->count += 1;
        return *slot;
    }

    T& push(T&& value) {
        this->grow_for_one_more();
        T* slot = new (this->data + this->count) T(std::move(value));
        this->count += 1;
        return *slot;
    }

    template <typename... Args>
    T& emplace(Args&&... args) {
        this->grow_for_one_more();
        T* slot = new (this->data + this->count) T(std::forward<Args>(args)...);
        this->count += 1;
        return *slot;
    }

    // Removes and returns the last element.
    T pop() {
        this->count -= 1;
        T value = std::move(this->data[this->count]);
        std::destroy_at(this->data + this->count);
        return value;
    }

    // Removes the element at `index`, shifting later elements down. O(n).
    void remove_at(size_t index) {
        for (size_t i = index + 1; i < this->count; ++i) {
            this->data[i - 1] = std::move(this->data[i]);
        }
        this->count -= 1;
        std::destroy_at(this->data + this->count);
    }

    // Removes the element at `index` by moving the last element into its
    // place. O(1), but does not preserve order.
    void remove_swap(size_t index) {
        this->count -= 1;
        if (index != this->count) {
            this->data[index] = std::move(this->data[this->count]);
        }
        std::destroy_at(this->data + this->count);
    }

    // --- Access -------------------------------------------------------------

    T& operator[](size_t index) { return this->data[index]; }
    const T& operator[](size_t index) const { return this->data[index]; }

    T& first() { return this->data[0]; }
    const T& first() const { return this->data[0]; }

    T& last() { return this->data[this->count - 1]; }
    const T& last() const { return this->data[this->count - 1]; }

    bool is_empty() const { return this->count == 0; }

    T* begin() { return this->data; }
    T* end() { return this->data + this->count; }
    const T* begin() const { return this->data; }
    const T* end() const { return this->data + this->count; }

    // --- Members ------------------------------------------------------------

    BaseAllocator* allocator = nullptr;
    T* data = nullptr;
    size_t count = 0;
    size_t capacity = 0;

private:
    static constexpr size_t minimum_capacity = 8;

    void grow_for_one_more() {
        if (this->count < this->capacity) {
            return;
        }
        size_t doubled = this->capacity * 2;
        this->reserve(doubled < minimum_capacity ? minimum_capacity : doubled);
    }
};
