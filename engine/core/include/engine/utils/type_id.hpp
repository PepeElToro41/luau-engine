#pragma once

#include "engine/defines.hpp"

// A process-wide numeric id per C++ type, without RTTI. Ids are handed out
// in first-use order starting at 1, so they are dense but not stable across
// runs: never serialize them.
using TypeId = usz;

namespace TYPE_ID {

inline TypeId next_type_id() {
    static TypeId counter = 1;
    return counter++;
}

// Id for T. Qualifiers and references are not stripped: `T`, `const T` and
// `T&` are distinct types, so pass the plain type.
template <typename T>
TypeId get() {
    static const TypeId id = next_type_id();
    return id;
}

} // namespace TYPE_ID
