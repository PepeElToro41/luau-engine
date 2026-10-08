#pragma once

#include "engine/defines.hpp"

// 64-bit FNV-1a. Fast, tiny and good enough for name lookups, content hashes
// and cache keys; not for anything adversarial.
//
//     u64 tag = HASH::fnv1a_str("forward");               // constexpr
//     u64 key = HASH::fnv1a(&desc, sizeof(desc));
//     key = HASH::fnv1a_append(key, formats, count * 4);   // chain more bytes
namespace HASH {

constexpr u64 FNV1A_OFFSET = 0xcbf29ce484222325ull;
constexpr u64 FNV1A_PRIME = 0x100000001b3ull;

// Folds `size` bytes into `hash`; fnv1a(data, size) is
// fnv1a_append(FNV1A_OFFSET, data, size).
u64 fnv1a_append(u64 hash, const void* data, usz size);
u64 fnv1a(const void* data, usz size);

// Over the characters of a null-terminated string, not including the
// terminator. Same value as fnv1a(text, strlen(text)).
constexpr u64 fnv1a_str_append(u64 hash, const char* text) {
    while (*text != '\0') {
        hash ^= static_cast<u8>(*text++);
        hash *= FNV1A_PRIME;
    }
    return hash;
}
constexpr u64 fnv1a_str(const char* text) {
    return fnv1a_str_append(FNV1A_OFFSET, text);
}

} // namespace HASH
