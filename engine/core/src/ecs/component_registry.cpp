#include "engine/ecs/component_registry.hpp"

#include "engine/utils/log.hpp"

#include <cstring>

namespace {

// The table: entry i holds id i + 1. Static, zero-initialized, so a claim
// before main works without any setup.
ComponentRegistryEntry entries[ECS::MAX_COMPONENT_ID];
usz entry_count = 0;

} // namespace

Id COMPONENT_REGISTRY::claim(const TypeId type_id, const TypeInfo info, const bool is_tag, const char* name) {
    if (type_id == 0) {
        return 0;
    }
    if (const ComponentRegistryEntry* existing = COMPONENT_REGISTRY::find(type_id)) {
        return existing->id;
    }
    if (entry_count >= ECS::MAX_COMPONENT_ID) {
        LOG::error(LOG_CATEGORY::ECS, "component registry: out of component ids (max %llu); cannot preregister type %llu%s%s",
            static_cast<unsigned long long>(ECS::MAX_COMPONENT_ID), static_cast<unsigned long long>(type_id), name != nullptr ? " " : "", name != nullptr ? name : "");
        return 0;
    }
    ComponentRegistryEntry& entry = entries[entry_count];
    entry.type_id = type_id;
    entry.id = ECS::FIRST_COMPONENT_ID + entry_count;
    entry.info = is_tag ? TypeInfo {} : info;
    entry.is_tag = is_tag;
    entry.name = name;
    entry_count += 1;
    return entry.id;
}

const ComponentRegistryEntry* COMPONENT_REGISTRY::find(const TypeId type_id) {
    for (usz i = 0; i < entry_count; ++i) {
        if (entries[i].type_id == type_id) {
            return &entries[i];
        }
    }
    return nullptr;
}

const ComponentRegistryEntry* COMPONENT_REGISTRY::find_id(const Id id) {
    if (id < ECS::FIRST_COMPONENT_ID || id - ECS::FIRST_COMPONENT_ID >= entry_count) {
        return nullptr;
    }
    return &entries[id - ECS::FIRST_COMPONENT_ID];
}

const ComponentRegistryEntry* COMPONENT_REGISTRY::find_name(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (usz i = 0; i < entry_count; ++i) {
        if (entries[i].name != nullptr && strcmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }
    return nullptr;
}

usz COMPONENT_REGISTRY::count() {
    return entry_count;
}

const ComponentRegistryEntry& COMPONENT_REGISTRY::entry(const usz index) {
    return entries[index];
}

void COMPONENT_REGISTRY::reset() {
    for (usz i = 0; i < entry_count; ++i) {
        entries[i] = ComponentRegistryEntry {};
    }
    entry_count = 0;
}
