#pragma once

#include "engine/defines.hpp"
#include "engine/templates/dynamic_array.hpp"

// Short ascending id lists, as kept per archetype for monitors and observers
// (see ArchetypeObservers). Ids are issued in increasing order, so insert()
// almost always appends; the lists are a few entries long, so everything is a
// linear walk.
namespace SORTED_IDS {

// Inserts `id` keeping the list sorted; a no-op if the id is already there.
template <typename T>
void insert(DynamicArray<T>& list, const T id) {
    usz position = list.count;
    while (position > 0 && list[position - 1] > id) {
        position--;
    }
    if (position > 0 && list[position - 1] == id) {
        return;
    }
    list.push(id);
    for (usz i = list.count - 1; i > position; i--) {
        list[i] = list[i - 1];
    }
    list[position] = id;
}

// Removes `id` if present, keeping the order.
template <typename T>
void remove(DynamicArray<T>& list, const T id) {
    for (usz i = 0; i < list.count; i++) {
        if (list[i] == id) {
            list.remove_at(i);
            return;
        }
        if (list[i] > id) {
            return;
        }
    }
}

// Whether `id` is in the list.
template <typename T>
bool contains(const DynamicArray<T>& list, const T id) {
    for (usz i = 0; i < list.count; i++) {
        if (list[i] == id) {
            return true;
        }
        if (list[i] > id) {
            return false;
        }
    }
    return false;
}

} // namespace SORTED_IDS
