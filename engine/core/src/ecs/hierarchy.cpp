#include "engine/ecs/hierarchy.hpp"

#include "engine/ecs/archetype.hpp"
#include "engine/ecs/component_record.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/world.hpp"

#include <cstdio>
#include <new>

namespace HIERARCHY {

namespace {

// Index of the first (relation, *) pair in the archetype's type, or
// `id_count` if it holds none. Pairs with the same relation are contiguous in
// the sorted type (the relation sits in the high bits), and the (R, *)
// wildcard alias points at the first of them.
usz first_pair_column(const Archetype* archetype, const EntityIdLow relation) {
    const usz* column = archetype->columns_index.find(ECS::PAIR(relation, ECS::WILDCARD));
    return column != nullptr ? *column : archetype->type.id_count;
}

// Whether `source` and `destination` hold the same (relation, *) pairs, in
// which case every entity moving between them keeps its parents and so its
// depth. The root holds none.
bool same_targets(const Archetype* source, const Archetype* destination, const EntityIdLow relation) {
    const ArchetypeType& source_type = source->type;
    const ArchetypeType& destination_type = destination->type;
    usz i = first_pair_column(source, relation);
    usz j = first_pair_column(destination, relation);
    while (true) {
        const bool source_has = i < source_type.id_count && ECS::PAIR_FIRST(source_type.ids[i]) == relation;
        const bool destination_has = j < destination_type.id_count && ECS::PAIR_FIRST(destination_type.ids[j]) == relation;
        if (!source_has || !destination_has) {
            return source_has == destination_has;
        }
        if (source_type.ids[i] != destination_type.ids[j]) {
            return false;
        }
        i++;
        j++;
    }
}

// Adds (`link`) or removes `node_record` from the children of the nodes of
// every (relation, *) pair `archetype` holds: the parents of an entity that
// lives there. Every pair in a type has a record, and a traversable one has
// a node.
void link_under_parents(World* world, ComponentRecord* node_record, const Archetype* archetype, const EntityIdLow relation, const bool link) {
    const ArchetypeType& type = archetype->type;
    for (usz i = first_pair_column(archetype, relation); i < type.id_count && ECS::PAIR_FIRST(type.ids[i]) == relation; i++) {
        ComponentRecord* parent = ComponentRecord::component_record_find(world, type.ids[i]);
        if (parent == nullptr || parent->hierarchy == nullptr) {
            continue;
        }
        if (link) {
            parent->hierarchy->children.insert(node_record->id, node_record);
        } else {
            parent->hierarchy->children.remove(node_record->id);
        }
    }
}

void set_target_flag(World* world, const EntityId target, const bool set) {
    EntityRecord* target_record = target != 0 ? world->entity_index.get_record_alive(target) : nullptr;
    if (target_record == nullptr) {
        return;
    }
    if (set) {
        target_record->flags |= ENTITY_RECORD_TRAVERSABLE_TARGET;
    } else {
        target_record->flags &= ~static_cast<u32>(ENTITY_RECORD_TRAVERSABLE_TARGET);
    }
}

} // namespace

u32 depth(World* world, ComponentRecord* record) {
    HierarchyNode* node = record->hierarchy;
    if (node == nullptr) {
        return 0;
    }
    if (!node->dirty) {
        return node->depth;
    }
    if (node->computing) {
        // Reached this pair again while walking up from it: its target is
        // its own ancestor. No depth is right; 0 closes the loop.
        fprintf(stderr, "[ecs] error: cycle in relation %llx through pair %llx; hierarchy depth is undefined\n",
            ECS::PAIR_FIRST(record->id), record->id);
        return 0;
    }
    node->computing = true;

    // Holders of (R, T) sit one below T. A dead or parentless T is depth 0.
    u32 target_depth = 0;
    EntityId target;
    const EntityRecord* target_record = world->entity_index.resolve_low(ECS::PAIR_SECOND(record->id), &target);
    if (target_record != nullptr && target_record->archetype != nullptr) {
        target_depth = depth(world, target_record->archetype, ECS::PAIR_FIRST(record->id));
    }

    node->depth = target_depth + 1;
    node->dirty = false;
    node->computing = false;
    return node->depth;
}

u32 depth(World* world, Archetype* archetype, const EntityIdLow relation) {
    // The deepest parent wins, so that a child always sorts after every one
    // of its parents.
    u32 deepest = 0;
    const ArchetypeType& type = archetype->type;
    for (usz i = first_pair_column(archetype, relation); i < type.id_count && ECS::PAIR_FIRST(type.ids[i]) == relation; i++) {
        ComponentRecord* record = ComponentRecord::component_record_find(world, type.ids[i]);
        if (record == nullptr) {
            continue;
        }
        const u32 pair_depth = depth(world, record);
        if (pair_depth > deepest) {
            deepest = pair_depth;
        }
    }
    return deepest;
}

u32 depth(World* world, const EntityId entity, const EntityIdLow relation) {
    const EntityRecord* record = world->entity_index.get_record_alive(entity);
    if (record == nullptr || record->archetype == nullptr) {
        return 0;
    }
    return depth(world, record->archetype, relation);
}

void invalidate(World* world, ComponentRecord* record) {
    HierarchyNode* node = record->hierarchy;
    if (node == nullptr || node->dirty) {
        // Already dirty means everything below it is too: a child is only
        // ever computed after its parents, and dirtying always propagates.
        return;
    }
    node->dirty = true;
    world->hierarchy_generation++;
    for (auto& child : node->children) {
        invalidate(world, child.value);
    }
}

void on_move(World* world, const EntityId entity, const EntityRecord* record, Archetype* source, Archetype* destination) {
    if ((record->flags & ENTITY_RECORD_TRAVERSABLE_TARGET) == 0 || source == destination) {
        return;
    }
    const ComponentRecord* as_target = ComponentRecord::component_record_find(world, ECS::PAIR(ECS::WILDCARD, entity));
    if (as_target == nullptr || as_target->pair_record == nullptr) {
        return;
    }

    // One relation at a time: the entity is a parent through each of these,
    // and its node for each is the (R, entity) record itself.
    for (auto& entry : as_target->pair_record->trav_records) {
        const EntityIdLow relation = ECS::PAIR_FIRST(entry.key);
        ComponentRecord* node_record = entry.value;
        // Same parents on both sides (the move added or removed something
        // unrelated): the node stays where it is and nothing below changed.
        if (same_targets(source, destination, relation)) {
            continue;
        }
        link_under_parents(world, node_record, source, relation, false);
        link_under_parents(world, node_record, destination, relation, true);
        // Different parents at the same depth (reparented under a sibling):
        // the subtree keeps its depths too.
        if (depth(world, source, relation) == depth(world, destination, relation)) {
            continue;
        }
        invalidate(world, node_record);
    }
}

void on_target_record_created(World* world, ComponentRecord* record) {
    if (record->hierarchy != nullptr) {
        return;
    }
    HierarchyNode* node = record->allocator->allocate_array<HierarchyNode>(1);
    new (node) HierarchyNode(record->allocator);
    record->hierarchy = node;

    // T can now have children; hang its node under its current parents.
    const EntityId target = world->pair_second(record->id);
    set_target_flag(world, target, true);
    const EntityRecord* target_record = target != 0 ? world->entity_index.get_record_alive(target) : nullptr;
    if (target_record != nullptr && target_record->archetype != nullptr) {
        link_under_parents(world, record, target_record->archetype, ECS::PAIR_FIRST(record->id), true);
    }
}

void on_target_record_deleted(World* world, ComponentRecord* record) {
    if (record->hierarchy == nullptr) {
        return;
    }
    const EntityId target = world->pair_second(record->id);
    const EntityRecord* target_record = target != 0 ? world->entity_index.get_record_alive(target) : nullptr;
    if (target_record != nullptr && target_record->archetype != nullptr) {
        link_under_parents(world, record, target_record->archetype, ECS::PAIR_FIRST(record->id), false);
    }
    // Still a target through some other traversable relation: keep the flag.
    const ComponentRecord* as_target = record->second_wildcard;
    const bool still_target = as_target != nullptr && as_target->pair_record != nullptr && !as_target->pair_record->trav_records.is_empty();
    if (!still_target) {
        set_target_flag(world, target, false);
    }
    // Nothing holds the pair any more (the record is being deleted), so no
    // child node is linked here; the map goes with the node either way.
    free_node(record);
}

void free_node(ComponentRecord* record) {
    HierarchyNode* node = record->hierarchy;
    if (node == nullptr) {
        return;
    }
    node->children.free();
    record->allocator->free(node);
    record->hierarchy = nullptr;
}

} // namespace HIERARCHY
