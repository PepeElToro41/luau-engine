#pragma once

#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/utils/type_id.hpp"

#include <cstdio>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

struct Engine;

// A plugin is a unit of engine setup: it creates singletons, registers
// components, adds phases and systems, and requires the plugins it builds
// on. The Engine owns an ordered list of them (engine.add_plugin<T>(args...)
// before engine.init()) and builds each once, in dependency order, from
// init(). There is no shutdown method: what a plugin tears down is systems
// it adds in build() to the engine's "shutdown" pipeline, whose phases
// order teardown without a plugin naming any other (see engine.hpp).
//
//     struct AudioPlugin : Plugin {
//         static constexpr const char* NAME = "audio";
//         bool build(Engine& engine) override {
//             CorePlugin* core = engine.require<CorePlugin>();   // built first if needed
//             if (core == nullptr) {
//                 return false;
//             }
//             AudioState* audio = engine.create_singleton<AudioState>();
//             engine.add_system(engine.phases.update, "audio mix", &mix, audio);
//             engine.add_system(engine.phases.shutdown, "audio", &shutdown, audio);
//             return true;
//         }
//     };
//
// Dependencies are declared where they are used: require<T>() inside
// build() returns T's plugin, building it first when it was added but not
// built yet, or nullptr (with a message) when it was never added. Plugins
// that require nothing of each other build in the order they were added;
// a cycle (A requires B requires A) is reported and fails init.
// get_plugin<T>() is the optional lookup: it never builds.
//
// Every plugin type carries `static constexpr const char* NAME`, used in
// messages, and is added at most once (keyed by TYPE_ID::get<T>()).
struct Plugin {
    // Once, from Engine::init(), after everything it requires. False fails
    // init; a plugin that fails should leave nothing behind.
    virtual bool build(Engine& engine) = 0;
};

enum PluginState : u32 {
    PLUGIN_ADDED = 0,
    PLUGIN_BUILDING = 1,
    PLUGIN_BUILT = 2,
    PLUGIN_FAILED = 3,
};

struct PluginEntry {
    TypeId type = 0;
    const char* name = "";
    Plugin* plugin = nullptr;
    // Runs the concrete type's destructor on `plugin` (nothing, for engine
    // types); the registry then frees the memory. Never through the base.
    void (*destroy)(Plugin* plugin) = nullptr;
    PluginState state = PLUGIN_ADDED;
};

// The Engine's plugin list. Plugins live in their own allocations, so the
// pointer add() / get() / require() hand out stays valid until free().
struct PluginRegistry {
    PluginRegistry();
    explicit PluginRegistry(BaseAllocator* allocator);

    PluginRegistry(const PluginRegistry&) = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

    // Constructs T(args...) and appends it. nullptr (with a message) if T
    // was added before.
    template <typename T, typename... Args>
    T* add(Args&&... args);
    // T's plugin, or nullptr if it was never added. Never builds.
    template <typename T>
    T* get() const;
    template <typename T>
    bool is_added() const;
    // T's plugin, built now if it was added and is not built yet. nullptr
    // (with a message) if T was never added, failed to build, or is being
    // built right now (a cycle).
    template <typename T>
    T* require(Engine& engine);

    // Builds every added plugin that is not built yet, in insertion order,
    // each one's requirements first. False if any failed.
    bool build_all(Engine& engine);
    // Destroys every plugin (as its concrete type) and releases the storage.
    void free();

    const PluginEntry* find(TypeId type) const;
    usz count() const { return this->entries.count; }

    BaseAllocator* allocator = nullptr;
    // In insertion order; indices are stable, pointers into it are not.
    DynamicArray<PluginEntry> entries;
    // Indices into `entries` in the order they finished building.
    DynamicArray<usz> build_order;

private:
    usz find_index(TypeId type) const;
    bool build_entry(Engine& engine, usz index);

    template <typename T>
    static void destroy_plugin(Plugin* plugin) {
        std::destroy_at(static_cast<T*>(plugin));
    }
};

template <typename T, typename... Args>
T* PluginRegistry::add(Args&&... args) {
    static_assert(std::is_base_of_v<Plugin, T>, "PluginRegistry::add<T>: T must derive from Plugin");
    static_assert(std::is_constructible_v<T, Args&&...>, "PluginRegistry::add<T>: T cannot be constructed from these arguments");

    const TypeId type = TYPE_ID::get<T>();
    if (this->find(type) != nullptr) {
        fprintf(stderr, "[plugins] plugin '%s' was already added\n", T::NAME);
        return nullptr;
    }
    T* plugin = this->allocator->allocate_array<T>(1);
    new (plugin) T(std::forward<Args>(args)...);

    PluginEntry entry;
    entry.type = type;
    entry.name = T::NAME;
    entry.plugin = plugin;
    entry.destroy = &PluginRegistry::destroy_plugin<T>;
    this->entries.push(entry);
    return plugin;
}

template <typename T>
T* PluginRegistry::get() const {
    const PluginEntry* entry = this->find(TYPE_ID::get<T>());
    return entry != nullptr ? static_cast<T*>(entry->plugin) : nullptr;
}

template <typename T>
bool PluginRegistry::is_added() const {
    return this->find(TYPE_ID::get<T>()) != nullptr;
}

template <typename T>
T* PluginRegistry::require(Engine& engine) {
    const usz index = this->find_index(TYPE_ID::get<T>());
    if (index == this->entries.count) {
        fprintf(stderr, "[plugins] plugin '%s' is required but was not added\n", T::NAME);
        return nullptr;
    }
    if (!this->build_entry(engine, index)) {
        return nullptr;
    }
    return static_cast<T*>(this->entries[index].plugin);
}
