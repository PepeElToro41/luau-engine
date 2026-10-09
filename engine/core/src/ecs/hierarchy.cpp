#include "engine/ecs/hierarchy.hpp"

#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/component_record.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"

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
// a node. The node's `parents` list is the mirror: filled on link, emptied
// on unlink.
void link_under_parents(World* world, ComponentRecord* node_record, const Archetype* archetype, const EntityIdLow relation, const bool link) {
    HierarchyNode* node = node_record->hierarchy;
    if (!link) {
        node->parents.clear();
    }
    const ArchetypeType& type = archetype->type;
    for (usz i = first_pair_column(archetype, relation); i < type.id_count && ECS::PAIR_FIRST(type.ids[i]) == relation; i++) {
        ComponentRecord* parent = archetype->records[i];
        // An exclusive relation has one pair per archetype: done after it.
        const bool last = parent->is_exclusive();
        if (parent->hierarchy == nullptr) {
            if (last) {
                break;
            }
            continue;
        }
        if (link) {
            parent->hierarchy->children.insert(node_record->id, node_record);
            node->parents.push(parent);
        } else {
            parent->hierarchy->children.remove(node_record->id);
        }
        if (last) {
            break;
        }
    }
}

// Merges `parent` into `own` (both sorted by id) with the parent's ranks
// offset by `rank_base`; an id in both keeps the own entry, the nearer one.
void merge_reachable(ReachableSet& own, const ReachableSet& parent, const u32 rank_base) {
    if (parent.count() == 0) {
        return;
    }
    const usz own_count = own.count();
    const usz parent_count = parent.count();
    TemporalAllocator temp = TemporalAllocator::create();
    Id* merged_ids = temp.allocate_array<Id>(own_count + parent_count + 1);
    ReachableEntry* merged = temp.allocate_array<ReachableEntry>(own_count + parent_count + 1);
    usz i = 0;
    usz j = 0;
    usz n = 0;
    while (i < own_count || j < parent_count) {
        if (j >= parent_count || (i < own_count && own.ids[i] < parent.ids[j])) {
            merged_ids[n] = own.ids[i];
            merged[n++] = own.entries[i++];
        } else if (i < own_count && own.ids[i] == parent.ids[j]) {
            merged_ids[n] = own.ids[i];
            merged[n++] = own.entries[i++];
            j++;
        } else {
            merged_ids[n] = parent.ids[j];
            merged[n] = parent.entries[j++];
            merged[n].rank += rank_base;
            n++;
        }
    }
    own.ids.resize(n);
    own.entries.resize(n);
    for (usz k = 0; k < n; k++) {
        own.ids[k] = merged_ids[k];
        own.entries[k] = merged[k];
    }
}

// Rebuilds the reachable set of `record`'s node: the target's own ids with
// rank 0, then each parent's set in order. The parents are brought up to
// date first, so this never recurses while holding scratch memory.
void compute_reachable(World* world, ComponentRecord* record) {
    HierarchyNode* node = record->hierarchy;
    node->reachable_computing = true;
    for (ComponentRecord* parent : node->parents) {
        reachable(world, parent);
    }

    node->reachable.clear();
    node->reachable_ranks = 1;
    EntityId target;
    const EntityRecord* target_record = world->entity_index.resolve_low(ECS::PAIR_SECOND(record->id), &target);
    const Archetype* archetype = node->target_archetype;
    if (target_record != nullptr && archetype != nullptr) {
        // A type is sorted by id, so the own entries come out sorted.
        const ArchetypeType& type = archetype->type;
        node->reachable.ids.reserve(type.id_count);
        node->reachable.entries.reserve(type.id_count);
        for (usz i = 0; i < type.id_count; i++) {
            node->reachable.push(type.ids[i], ReachableEntry { target, static_cast<u32>(i), 0 });
        }
    }
    for (ComponentRecord* parent : node->parents) {
        const HierarchyNode* parent_node = parent->hierarchy;
        // Still dirty after being asked: the parent is computing further
        // up the stack, i.e. this is the link closing a cycle.
        if (parent_node == nullptr || parent_node->reachable_dirty) {
            continue;
        }
        merge_reachable(node->reachable, parent_node->reachable, node->reachable_ranks);
        node->reachable_ranks += parent_node->reachable_ranks;
    }
    node->reachable_dirty = false;
    node->reachable_computing = false;
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
    // of its parents (IS_A allows several); an exclusive relation (CHILD_OF)
    // has exactly one pair in the type, so the loop ends after it.
    u32 deepest = 0;
    const ArchetypeType& type = archetype->type;
    for (usz i = first_pair_column(archetype, relation); i < type.id_count && ECS::PAIR_FIRST(type.ids[i]) == relation; i++) {
        ComponentRecord* record = archetype->records[i];
        const u32 pair_depth = depth(world, record);
        if (pair_depth > deepest) {
            deepest = pair_depth;
        }
        if (record->is_exclusive()) {
            break;
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

void order_by_depth(World* world, Archetype* const* archetypes, const usz count, const EntityIdLow relation, const bool descending, u32* order) {
    TemporalAllocator temp = TemporalAllocator::create();
    u32* depths = temp.allocate_array<u32>(count + 1);
    u32 deepest = 0;
    for (usz i = 0; i < count; i++) {
        depths[i] = depth(world, archetypes[i], relation);
        if (depths[i] > deepest) {
            deepest = depths[i];
        }
    }

    // Counting sort on the depth (mirrored for descending): offsets[k]
    // ends up as the first slot of key k, then advances as it is filled.
    usz* offsets = temp.allocate_array<usz>(deepest + 2);
    for (usz k = 0; k <= deepest + 1; k++) {
        offsets[k] = 0;
    }
    for (usz i = 0; i < count; i++) {
        const u32 key = descending ? deepest - depths[i] : depths[i];
        offsets[key + 1]++;
    }
    for (usz k = 1; k <= deepest + 1; k++) {
        offsets[k] += offsets[k - 1];
    }
    for (usz i = 0; i < count; i++) {
        const u32 key = descending ? deepest - depths[i] : depths[i];
        order[offsets[key]++] = static_cast<u32>(i);
    }
}

const ReachableSet* reachable(World* world, ComponentRecord* record) {
    HierarchyNode* node = record->hierarchy;
    if (node == nullptr) {
        return nullptr;
    }
    if (node->reachable_computing) {
        fprintf(stderr, "[ecs] error: cycle in relation %llx through pair %llx; what its holders reach is undefined\n",
            static_cast<unsigned long long>(ECS::PAIR_FIRST(record->id)), static_cast<unsigned long long>(record->id));
        return nullptr;
    }
    if (node->reachable_dirty) {
        compute_reachable(world, record);
    }
    return &node->reachable;
}

usz find_reachable(const ReachableSet& set, const Id pattern, const usz start) {
    const usz count = set.ids.count;
    const Id* ids = set.ids.data;
    if (!ECS::IS_PAIR(pattern) ? !ECS::IS_WILDCARD(pattern) : !ECS::PAIR_HAS_WILDCARD(pattern)) {
        // Concrete: at most one entry, found by binary search; it counts
        // only if it sits at or after `start`.
        usz low = 0;
        usz high = count;
        while (low < high) {
            const usz middle = (low + high) / 2;
            if (ids[middle] < pattern) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        return low < count && low >= start && ids[low] == pattern ? low : count;
    }

    usz i = start;
    // (R, *): the pairs of R are contiguous, so skip to the first of them
    // and stop past the last.
    const bool relation_range = ECS::IS_PAIR(pattern) && !ECS::IS_WILDCARD(ECS::PAIR_FIRST(pattern)) && ECS::IS_WILDCARD(ECS::PAIR_SECOND(pattern));
    if (relation_range) {
        const Id lowest = ECS::PAIR(ECS::PAIR_FIRST(pattern), 0);
        usz low = i;
        usz high = count;
        while (low < high) {
            const usz middle = (low + high) / 2;
            if (ids[middle] < lowest) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        i = low;
    }
    for (; i < count; i++) {
        const Id id = ids[i];
        if (relation_range && (!ECS::IS_PAIR(id) || ECS::PAIR_FIRST(id) != ECS::PAIR_FIRST(pattern))) {
            return count;
        }
        if (ECS::ID_MATCHES(pattern, id)) {
            return i;
        }
    }
    return count;
}

void invalidate_reachable(ComponentRecord* record) {
    HierarchyNode* node = record->hierarchy;
    if (node == nullptr || node->reachable_dirty) {
        // A node is only computed after its parents and dirtying always
        // propagates, so a dirty node has a dirty subtree.
        return;
    }
    node->reachable_dirty = true;
    for (auto& child : node->children) {
        invalidate_reachable(child.value);
    }
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
        // Whatever moved, the entity's ids changed, and with them what its
        // children reach.
        if (node_record->hierarchy != nullptr) {
            node_record->hierarchy->target_archetype = destination;
            invalidate_reachable(node_record);
        }
        // Same parents on both sides (the move added or removed something
        // unrelated): the node stays where it is and no depth below changed.
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
        node->target_archetype = target_record->archetype;
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
    node->parents.free();
    node->reachable.free();
    record->allocator->free(node);
    record->hierarchy = nullptr;
}

} // namespace HIERARCHY
