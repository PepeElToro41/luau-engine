#include "engine/utils/hash.hpp"

u64 HASH::fnv1a_append(u64 hash, const void* data, const usz size) {
    const u8* bytes = static_cast<const u8*>(data);
    for (usz i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV1A_PRIME;
    }
    return hash;
}

u64 HASH::fnv1a(const void* data, const usz size) {
    return fnv1a_append(FNV1A_OFFSET, data, size);
}
