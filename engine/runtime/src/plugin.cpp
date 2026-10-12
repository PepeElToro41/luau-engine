#include "engine/plugin.hpp"

#include "engine/memory/heap_allocator.hpp"

#include <cstdio>

PluginRegistry::PluginRegistry() : PluginRegistry(MEMORY::heap_allocator()) {}

PluginRegistry::PluginRegistry(BaseAllocator* allocator) : allocator(allocator), entries(allocator), build_order(allocator) {}

const PluginEntry* PluginRegistry::find(const TypeId type) const {
    const usz index = this->find_index(type);
    return index != this->entries.count ? &this->entries[index] : nullptr;
}

usz PluginRegistry::find_index(const TypeId type) const {
    for (usz i = 0; i < this->entries.count; ++i) {
        if (this->entries[i].type == type) {
            return i;
        }
    }
    return this->entries.count;
}

bool PluginRegistry::build_entry(Engine& engine, const usz index) {
    switch (this->entries[index].state) {
    case PLUGIN_BUILT:
        return true;
    case PLUGIN_FAILED:
        return false;
    case PLUGIN_BUILDING:
        fprintf(stderr, "[plugins] plugin '%s' requires itself through a cycle\n", this->entries[index].name);
        return false;
    case PLUGIN_ADDED:
        break;
    }
    this->entries[index].state = PLUGIN_BUILDING;
    // The entry array may grow if a build adds plugins, so nothing is kept
    // across the call but the index.
    const bool built = this->entries[index].plugin->build(engine);
    this->entries[index].state = built ? PLUGIN_BUILT : PLUGIN_FAILED;
    if (built) {
        this->build_order.push(index);
    } else {
        fprintf(stderr, "[plugins] plugin '%s' failed to build\n", this->entries[index].name);
    }
    return built;
}

bool PluginRegistry::build_all(Engine& engine) {
    bool ok = true;
    for (usz i = 0; i < this->entries.count; ++i) {
        if (!this->build_entry(engine, i)) {
            ok = false;
        }
    }
    return ok;
}

void PluginRegistry::free() {
    for (PluginEntry& entry : this->entries) {
        entry.destroy(entry.plugin);
        this->allocator->free(entry.plugin);
        entry.plugin = nullptr;
    }
    this->entries.free();
    this->build_order.free();
}
