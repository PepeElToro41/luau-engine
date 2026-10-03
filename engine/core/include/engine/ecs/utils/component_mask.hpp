#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"

#include <bit>

// Number of 64-bit words needed to give every component id its own bit.
constexpr usz COMPONENT_MASK_WORDS = (ECS::MAX_COMPONENT_ID + 63) / 64;
static_assert(COMPONENT_MASK_WORDS <= 64, "ComponentMask::outer has one bit per word, so at most 64 words fit");

// Two-level bitset over the component id range [1, ECS::MAX_COMPONENT_ID].
//
// `words` has one bit per component id; `outer` has one bit per word and is
// set when that word is non-zero. Set tests walk only the words the other
// mask's `outer` names, so a query over three components touches at most
// three words no matter how large the range is, and the `outer` compare
// alone rejects most mismatches.
//
// Pairs, built-in ids (ECS::WILDCARD and up) and tag entities do not fit:
// check fits() first and route those through the bloom filter / id list.
struct ComponentMask {
    u64 outer = 0;
    u64 words[COMPONENT_MASK_WORDS] = {};

    static constexpr bool fits(const Id id) {
        return id >= 1 && id <= ECS::MAX_COMPONENT_ID;
    }

    void set(const Id id) {
        const u64 bit = id - 1;
        const u64 word = bit >> 6;
        this->words[word] |= 1ull << (bit & 63);
        this->outer |= 1ull << word;
    }

    bool has(const Id id) const {
        const u64 bit = id - 1;
        return (this->words[bit >> 6] >> (bit & 63)) & 1ull;
    }

    bool is_empty() const { return this->outer == 0; }

    // Every bit set in `other` is also set here. An empty `other` always passes.
    bool contains_all(const ComponentMask& other) const {
        if ((this->outer & other.outer) != other.outer) {
            return false;
        }
        u64 remaining = other.outer;
        while (remaining != 0) {
            const usz word = std::countr_zero(remaining);
            remaining &= remaining - 1;
            if ((this->words[word] & other.words[word]) != other.words[word]) {
                return false;
            }
        }
        return true;
    }

    // At least one bit is set in both masks.
    bool intersects(const ComponentMask& other) const {
        u64 shared = this->outer & other.outer;
        while (shared != 0) {
            const usz word = std::countr_zero(shared);
            shared &= shared - 1;
            if ((this->words[word] & other.words[word]) != 0) {
                return true;
            }
        }
        return false;
    }
};
