#include "engine/asset/asset_types/scene_asset.hpp"

#include "engine/asset/asset_writer.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/scene/scene.hpp"
#include "engine/templates/hash_map.hpp"
#include "engine/utils/log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// --- Writing ------------------------------------------------------------------

namespace {

struct EntityIdHash {
    usz operator()(const EntityId id) const { return static_cast<usz>(id * 0x9e3779b97f4a7c15ull); }
};

struct SceneWriteState {
    HashMap<EntityId, EntityIdLow, EntityIdHash> local_ids;
};

EntityIdLow local_id_of(const EntityId entity, void* scene_data) {
    const SceneWriteState* state = static_cast<const SceneWriteState*>(scene_data);
    const EntityIdLow* local = state->local_ids.find(entity);
    return local != nullptr ? *local : 0;
}

// Appends the subtree under `parent` in depth-first order, with the local
// id of each entity's parent (0 for the root's children).
void collect(World& world, const EntityId parent, const EntityIdLow parent_local, DynamicArray<EntityId>& order, DynamicArray<EntityIdLow>& parents,
             DynamicArray<EntityId>& stack) {
    const usz start = stack.count;
    const usz count = SCENE::children(world, parent, stack);
    for (usz i = 0; i < count; ++i) {
        const EntityId child = stack[start + i];
        order.push(child);
        parents.push(parent_local);
        collect(world, child, static_cast<EntityIdLow>(order.count), order, parents, stack);
    }
    stack.count = start;
}

struct Exposed {
    Id id;
    Serializer serializer;
};

void write_entity(World& world, const EntityId entity, const EntityIdLow local, const EntityIdLow parent_local, SerializeContext& ctx, TextWriter& writer) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "\n[entity %llu]\n", static_cast<unsigned long long>(local));
    writer.text(buffer);
    writer.prefix = nullptr;
    const char* name = SCENE::name(world, entity);
    if (name != nullptr && name[0] != '\0') {
        writer.field("name");
        writer.text(name);
        writer.end_line();
    }
    if (parent_local != 0) {
        snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(parent_local));
        writer.field("parent");
        writer.text(buffer);
        writer.end_line();
    }

    const EntityRecord* record = world.entity_index.get_record_alive(entity);
    if (record == nullptr || record->archetype == nullptr) {
        return;
    }
    // Copied out: the serializers are read from the component entities,
    // which could move the archetype's storage.
    const ArchetypeType& type = record->archetype->type;
    const usz count = type.id_count < ECS::MAX_COMPONENT_ID ? type.id_count : ECS::MAX_COMPONENT_ID;
    Id ids[ECS::MAX_COMPONENT_ID];
    memcpy(ids, type.ids, count * sizeof(Id));

    Exposed components[ECS::MAX_COMPONENT_ID];
    usz component_count = 0;
    Exposed tags[ECS::MAX_COMPONENT_ID];
    usz tag_count = 0;
    const Id name_id = world.id<Name>();
    for (usz i = 0; i < count; ++i) {
        const Id id = ids[i];
        if (id == name_id || ECS::IS_PAIR(id)) {
            continue;
        }
        const Serializer* serializer = SERIALIZE::of(world, id);
        if (serializer == nullptr) {
            continue;
        }
        if (serializer->is_tag()) {
            tags[tag_count++] = {id, *serializer};
        } else {
            components[component_count++] = {id, *serializer};
        }
    }
    // Lowest order first, ties by name: the layout then depends on nothing
    // but the file's own words, never on the order the components were
    // registered in (component ids are an implementation detail).
    const auto by_order = [](const Exposed& a, const Exposed& b) {
        if (a.serializer.order != b.serializer.order) {
            return a.serializer.order < b.serializer.order;
        }
        return strcmp(a.serializer.name, b.serializer.name) < 0;
    };
    std::sort(components, components + component_count, by_order);
    std::sort(tags, tags + tag_count, by_order);

    ctx.entity = entity;
    for (usz i = 0; i < component_count; ++i) {
        const void* data = world.get(entity, components[i].id);
        if (data == nullptr) {
            continue;
        }
        writer.prefix = components[i].serializer.name;
        components[i].serializer.write(ctx, data, writer);
    }
    writer.prefix = nullptr;
    if (tag_count > 0) {
        writer.field("tags");
        for (usz i = 0; i < tag_count; ++i) {
            if (i > 0) {
                writer.character(' ');
            }
            writer.text(tags[i].serializer.name);
        }
        writer.end_line();
    }
}

} // namespace

void SCENE_ASSET::write(World& world, const EntityId root, const AssetGuid& guid, SerializeContext& ctx, DynamicArray<char>& out) {
    TextWriter writer;
    writer.out = &out;
    writer.text("guid = ");
    writer.guid(guid);
    writer.end_line();

    DynamicArray<EntityId> order;
    DynamicArray<EntityIdLow> parents;
    DynamicArray<EntityId> stack;
    if (world.alive(root)) {
        collect(world, root, 0, order, parents, stack);
    }
    stack.free();

    SceneWriteState state;
    for (usz i = 0; i < order.count; ++i) {
        state.local_ids.insert(order[i], static_cast<EntityIdLow>(i + 1));
    }
    ctx.world = &world;
    ctx.local_id_of = &local_id_of;
    ctx.scene_data = &state;
    ctx.line = 0;
    for (usz i = 0; i < order.count; ++i) {
        write_entity(world, order[i], static_cast<EntityIdLow>(i + 1), parents[i], ctx, writer);
    }
    ctx.local_id_of = nullptr;
    ctx.scene_data = nullptr;
    ctx.entity = 0;
    state.local_ids.free();
    parents.free();
    order.free();
}

bool SCENE_ASSET::write_file(World& world, const EntityId root, const AssetGuid& guid, SerializeContext& ctx, const char* path) {
    if (guid.is_null()) {
        LOG::error(LOG_CATEGORY::SCENE, "cannot write %s: the scene has no guid", path != nullptr ? path : "(null)");
        return false;
    }
    DynamicArray<char> text;
    write(world, root, guid, ctx, text);
    const bool ok = ASSET_FILE::write_file(path, text.data, text.count);
    if (!ok) {
        LOG::error(LOG_CATEGORY::SCENE, "cannot write %s", path != nullptr ? path : "(null)");
    }
    text.free();
    return ok;
}

// --- Reading ------------------------------------------------------------------

namespace {

struct LocalIdHash {
    usz operator()(const EntityIdLow id) const { return static_cast<usz>(id * 0x9e3779b97f4a7c15ull); }
};

struct SceneReadState {
    HashMap<EntityIdLow, EntityId, LocalIdHash> entities;
};

EntityId entity_of_local_id(const EntityIdLow id, void* scene_data) {
    const SceneReadState* state = static_cast<const SceneReadState*>(scene_data);
    const EntityId* entity = state->entities.find(id);
    return entity != nullptr ? *entity : 0;
}

// One `[entity N]` as pass one saw it.
struct Section {
    EntityIdLow local = 0;
    const char* name = nullptr;
    usz name_length = 0;
    EntityIdLow parent = 0;
    u32 line = 0;
    EntityId entity = 0;
};

// `entity N` -> N; false for any other section name or a bad number.
bool parse_entity_section(const char* section, const usz length, EntityIdLow* out) {
    const char* word = nullptr;
    usz word_length = 0;
    usz offset = 0;
    if (!TEXT_ASSET::next_word(section, length, &offset, &word, &word_length) || !TEXT_ASSET::word_is(word, word_length, "entity")) {
        return false;
    }
    if (!TEXT_ASSET::next_word(section, length, &offset, &word, &word_length)) {
        return false;
    }
    for (usz i = 0; i < word_length; ++i) {
        if (word[i] < '0' || word[i] > '9') {
            return false;
        }
    }
    char buffer[32];
    if (word_length > 20 || !TEXT_ASSET::copy_span(word, word_length, buffer, sizeof(buffer))) {
        return false;
    }
    const EntityIdLow number = static_cast<EntityIdLow>(strtoull(buffer, nullptr, 10));
    const char* rest = nullptr;
    usz rest_length = 0;
    if (number == 0 || TEXT_ASSET::next_word(section, length, &offset, &rest, &rest_length)) {
        return false;
    }
    *out = number;
    return true;
}

bool parse_local_id(const char* value, const usz length, EntityIdLow* out) {
    if (length == 0 || length > 20) {
        return false;
    }
    for (usz i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    char buffer[32];
    TEXT_ASSET::copy_span(value, length, buffer, sizeof(buffer));
    *out = static_cast<EntityIdLow>(strtoull(buffer, nullptr, 10));
    return true;
}

Section* section_of(DynamicArray<Section>& sections, const EntityIdLow local) {
    for (usz i = 0; i < sections.count; ++i) {
        if (sections[i].local == local) {
            return &sections[i];
        }
    }
    return nullptr;
}

SCENE_ASSET::SceneReadResult fail(const char* error, const u32 line) {
    SCENE_ASSET::SceneReadResult result;
    result.ok = false;
    result.error = error;
    result.line = line;
    return result;
}

// Makes sure `entity` holds component `id`, set to its serializer's
// default when it was missing, and returns its data.
void* ensure_component(World& world, const EntityId entity, const Id id, const Serializer& serializer) {
    if (!world.has(entity, id)) {
        const TypeInfo* info = static_cast<const TypeInfo*>(world.get(id, ECS::COMPONENT));
        if (info == nullptr || info->length == 0) {
            return nullptr;
        }
        TemporalAllocator temp = TemporalAllocator::create();
        void* data = temp.allocate(info->length, info->alignment);
        memset(data, 0, info->length);
        if (serializer.init != nullptr) {
            serializer.init(data);
        }
        world.set(entity, id, data);
    }
    return world.get(entity, id);
}

void read_tags(World& world, const EntityId entity, const TextAssetEntry& entry, SerializeContext& ctx) {
    usz offset = 0;
    const char* word = nullptr;
    usz word_length = 0;
    while (TEXT_ASSET::next_word(entry.value, entry.value_length, &offset, &word, &word_length)) {
        const Id id = SERIALIZE::find(world, word, word_length);
        const Serializer* serializer = id != 0 ? SERIALIZE::of(world, id) : nullptr;
        if (serializer == nullptr) {
            SERIALIZE::warn(ctx, "unknown tag '%.*s'", static_cast<int>(word_length), word);
            continue;
        }
        if (!serializer->is_tag()) {
            SERIALIZE::warn(ctx, "'%.*s' is a component, not a tag", static_cast<int>(word_length), word);
            continue;
        }
        world.add(entity, id);
    }
}

void read_field(World& world, const EntityId entity, const TextAssetEntry& entry, SerializeContext& ctx) {
    const char* dot = static_cast<const char*>(memchr(entry.key, '.', entry.key_length));
    if (dot == nullptr) {
        SERIALIZE::warn(ctx, "unknown key '%.*s'", static_cast<int>(entry.key_length), entry.key);
        return;
    }
    const char* component = entry.key;
    const usz component_length = static_cast<usz>(dot - entry.key);
    const char* field = dot + 1;
    const usz field_length = entry.key_length - component_length - 1;
    const Id id = SERIALIZE::find(world, component, component_length);
    const Serializer* found = id != 0 ? SERIALIZE::of(world, id) : nullptr;
    if (found == nullptr) {
        SERIALIZE::warn(ctx, "unknown component '%.*s'", static_cast<int>(component_length), component);
        return;
    }
    if (found->is_tag()) {
        SERIALIZE::warn(ctx, "'%.*s' is a tag and has no fields", static_cast<int>(component_length), component);
        return;
    }
    // A copy: ensure_component may move the component entity's storage.
    const Serializer serializer = *found;
    void* data = ensure_component(world, entity, id, serializer);
    if (data == nullptr) {
        SERIALIZE::warn(ctx, "'%.*s' has no data to read into", static_cast<int>(component_length), component);
        return;
    }
    if (!serializer.read(ctx, data, field, field_length, entry.value, entry.value_length)) {
        SERIALIZE::warn(ctx, "%.*s: unknown field or bad value '%.*s'", static_cast<int>(entry.key_length), entry.key, static_cast<int>(entry.value_length),
            entry.value);
    }
}

} // namespace

SCENE_ASSET::SceneReadResult SCENE_ASSET::read(World& world, const char* text, const usz size, const EntityId parent, SerializeContext& ctx) {
    if ((text == nullptr && size != 0) || !world.alive(parent)) {
        return fail(text == nullptr && size != 0 ? "no text" : "the parent entity is not alive", 0);
    }
    const usz warnings_before = ctx.warnings;
    ctx.world = &world;
    ctx.entity = 0;
    ctx.line = 0;

    // Pass one: the structure. Every section, its name and its parent;
    // nothing is created until the whole file is known to be sound.
    DynamicArray<Section> sections;
    SCENE_ASSET::SceneReadResult result;
    bool has_guid = false;
    Section* current = nullptr;
    bool in_unknown_section = false;
    const char* last_section = nullptr;
    TextAssetCursor cursor(text, size);
    TextAssetEntry entry;
    while (cursor.next(&entry)) {
        ctx.line = entry.line;
        if (entry.in_top_level()) {
            if (entry.key_is("guid")) {
                if (has_guid) {
                    sections.free();
                    return fail("duplicate guid", entry.line);
                }
                if (!TEXT_ASSET::parse_guid(entry.value, entry.value_length, &result.guid)) {
                    sections.free();
                    return fail("malformed guid (32 hex digits expected)", entry.line);
                }
                has_guid = true;
            } else {
                SERIALIZE::warn(ctx, "unknown top-level key '%.*s'", static_cast<int>(entry.key_length), entry.key);
            }
            continue;
        }
        if (entry.section != last_section) {
            // A new section header was crossed.
            last_section = entry.section;
            current = nullptr;
            in_unknown_section = false;
            EntityIdLow local = 0;
            if (!parse_entity_section(entry.section, entry.section_length, &local)) {
                in_unknown_section = true;
                SERIALIZE::warn(ctx, "unknown section [%.*s]; its lines are skipped", static_cast<int>(entry.section_length), entry.section);
            } else if (section_of(sections, local) != nullptr) {
                sections.free();
                return fail("duplicate [entity N]", entry.line);
            } else {
                Section section;
                section.local = local;
                section.line = entry.line;
                sections.push(section);
                current = &sections[sections.count - 1];
            }
        }
        if (in_unknown_section || current == nullptr) {
            continue;
        }
        if (entry.key_is("name")) {
            current->name = entry.value;
            current->name_length = entry.value_length;
        } else if (entry.key_is("parent")) {
            if (!parse_local_id(entry.value, entry.value_length, &current->parent)) {
                SERIALIZE::warn(ctx, "bad parent '%.*s'", static_cast<int>(entry.value_length), entry.value);
                current->parent = 0;
            }
        }
    }
    if (cursor.error != nullptr) {
        sections.free();
        return fail(cursor.error, cursor.line);
    }
    if (!has_guid) {
        sections.free();
        return fail("no top-level guid", 0);
    }
    // A section with no `key = value` line at all (just the header) is
    // never reported by the cursor; such an entity would be nameless and
    // empty, which is fine to lose.

    // The entities, in file order, so a saved tree comes back with its
    // children in the order they were written (SCENE::children sorts by id).
    SceneReadState state;
    for (usz i = 0; i < sections.count; ++i) {
        Section& section = sections[i];
        section.entity = world.new_entity();
        if (section.entity == 0) {
            for (usz j = 0; j < i; ++j) {
                world.delete_entity(sections[j].entity);
            }
            state.entities.free();
            sections.free();
            return fail("no entity could be created (index exhausted)", section.line);
        }
        if (section.name_length > 0) {
            char name[ENTITY_NAME_CAPACITY];
            TEXT_ASSET::copy_span(section.name, section.name_length, name, sizeof(name));
            SCENE::set_name(world, section.entity, name);
        }
        state.entities.insert(section.local, section.entity);
    }
    for (usz i = 0; i < sections.count; ++i) {
        const Section& section = sections[i];
        EntityId target = parent;
        if (section.parent != 0) {
            const EntityId* found = state.entities.find(section.parent);
            if (found != nullptr && *found != section.entity) {
                target = *found;
            } else {
                ctx.line = section.line;
                SERIALIZE::warn(ctx, "parent %llu is not an entity of this file; parented under the load target instead",
                    static_cast<unsigned long long>(section.parent));
            }
        }
        SCENE::set_parent(world, section.entity, target);
    }

    // Pass two: components and tags, now that every reference can resolve.
    ctx.entity_of_local_id = &entity_of_local_id;
    ctx.scene_data = &state;
    cursor = TextAssetCursor(text, size);
    last_section = nullptr;
    current = nullptr;
    while (cursor.next(&entry)) {
        if (entry.in_top_level()) {
            continue;
        }
        if (entry.section != last_section) {
            last_section = entry.section;
            EntityIdLow local = 0;
            current = parse_entity_section(entry.section, entry.section_length, &local) ? section_of(sections, local) : nullptr;
        }
        if (current == nullptr || entry.key_is("name") || entry.key_is("parent")) {
            continue;
        }
        ctx.entity = current->entity;
        ctx.line = entry.line;
        if (entry.key_is("tags")) {
            read_tags(world, current->entity, entry, ctx);
        } else {
            read_field(world, current->entity, entry, ctx);
        }
    }
    ctx.entity_of_local_id = nullptr;
    ctx.scene_data = nullptr;
    ctx.entity = 0;
    ctx.line = 0;

    result.ok = true;
    result.entities = sections.count;
    result.warnings = ctx.warnings - warnings_before;
    state.entities.free();
    sections.free();
    return result;
}
