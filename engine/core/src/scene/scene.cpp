#include "engine/scene/scene.hpp"

#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/world.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

// --- Name --------------------------------------------------------------------

Name Name::make(const char* text) {
    Name name;
    if (text != nullptr) {
        strncpy(name.value, text, ENTITY_NAME_CAPACITY - 1);
        name.value[ENTITY_NAME_CAPACITY - 1] = '\0';
    }
    return name;
}

// --- Hierarchy ---------------------------------------------------------------

EntityId SCENE::parent(World& world, const EntityId entity) {
    const EntityRecord* record = world.entity_index.get_record_alive(entity);
    if (record == nullptr || record->archetype == nullptr) {
        return 0;
    }
    // CHILD_OF is exclusive, so the archetype holds at most one such pair.
    const ArchetypeType& type = record->archetype->type;
    for (usz i = 0; i < type.id_count; i++) {
        const Id id = type.ids[i];
        if (ECS::IS_PAIR(id) && ECS::PAIR_FIRST(id) == ECS::CHILD_OF) {
            return world.pair_second(id);
        }
    }
    return 0;
}

void SCENE::set_parent(World& world, const EntityId entity, const EntityId parent) {
    if (!world.alive(entity)) {
        return;
    }
    if (parent == 0) {
        const EntityId current = SCENE::parent(world, entity);
        if (current != 0) {
            world.remove(entity, ECS::PAIR(ECS::CHILD_OF, current));
        }
        return;
    }
    if (!world.alive(parent)) {
        return;
    }
    if (parent == entity || SCENE::is_descendant_of(world, parent, entity)) {
        fprintf(stderr, "[scene] warning: cannot parent entity %llx under %llx, it would close a cycle\n",
            static_cast<unsigned long long>(entity), static_cast<unsigned long long>(parent));
        return;
    }
    // Exclusive: add() swaps the current (CHILD_OF, *) for this one.
    world.add(entity, ECS::PAIR(ECS::CHILD_OF, parent));
}

bool SCENE::is_descendant_of(World& world, const EntityId entity, const EntityId ancestor) {
    if (entity == 0 || ancestor == 0 || !world.alive(ancestor)) {
        return false;
    }
    EntityId current = SCENE::parent(world, entity);
    while (current != 0) {
        if (current == ancestor) {
            return true;
        }
        current = SCENE::parent(world, current);
    }
    return false;
}

usz SCENE::children(World& world, const EntityId parent, DynamicArray<EntityId>& out) {
    if (!world.alive(parent)) {
        return 0;
    }
    const usz start = out.count;
    world.query<>().with(ECS::PAIR(ECS::CHILD_OF, parent)).each([&](const EntityId child) {
        out.push(child);
    });
    const usz added = out.count - start;
    std::sort(out.data + start, out.data + out.count, [](const EntityId a, const EntityId b) {
        return ECS::ENTITY_LOW(a) < ECS::ENTITY_LOW(b);
    });
    return added;
}

usz SCENE::child_count(World& world, const EntityId parent) {
    if (!world.alive(parent)) {
        return 0;
    }
    return world.query<>().with(ECS::PAIR(ECS::CHILD_OF, parent)).count();
}

bool SCENE::has_children(World& world, const EntityId parent) {
    if (!world.alive(parent)) {
        return false;
    }
    return !world.query<>().with(ECS::PAIR(ECS::CHILD_OF, parent)).empty();
}

// --- Names -------------------------------------------------------------------

const char* SCENE::name(World& world, const EntityId entity) {
    if (!world.alive(entity)) {
        return nullptr;
    }
    const Name* name = world.get<Name>(entity);
    return name != nullptr ? name->value : nullptr;
}

void SCENE::set_name(World& world, const EntityId entity, const char* name) {
    if (!world.alive(entity)) {
        return;
    }
    world.set(entity, Name::make(name));
}

EntityId SCENE::spawn(World& world, const char* name, const EntityId parent) {
    const EntityId entity = world.new_entity();
    if (entity == 0) {
        return 0;
    }
    world.set(entity, Name::make(name));
    if (parent != 0) {
        SCENE::set_parent(world, entity, parent);
    }
    return entity;
}

// --- Scene -------------------------------------------------------------------

void Scene::init(World* world) {
    this->world = world;
    this->root = SCENE::spawn(*world, "scene_root", 0);
}

EntityId Scene::spawn(const char* name, const EntityId parent) {
    return SCENE::spawn(*this->world, name, parent != 0 ? parent : this->root);
}

bool Scene::contains(const EntityId entity) {
    if (entity == 0 || this->root == 0) {
        return false;
    }
    return entity == this->root || SCENE::is_descendant_of(*this->world, entity, this->root);
}
