#include "engine/scene/serialize.hpp"

#include "engine/ecs/component_registry.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/utils/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

// --- TextWriter ---------------------------------------------------------------

void TextWriter::text(const char* text) {
    for (const char* c = text; *c != '\0'; ++c) {
        this->out->push(*c);
    }
}

void TextWriter::span(const char* text, const usz length) {
    for (usz i = 0; i < length; ++i) {
        this->out->push(text[i]);
    }
}

void TextWriter::character(const char c) {
    this->out->push(c);
}

void TextWriter::number(const f64 value) {
    char buffer[32];
    TEXT_ASSET::format_number(value, buffer, sizeof(buffer));
    this->text(buffer);
}

void TextWriter::guid(const AssetGuid& guid) {
    char buffer[TEXT_ASSET::GUID_TEXT_CAPACITY];
    TEXT_ASSET::format_guid(guid, buffer);
    this->text(buffer);
}

void TextWriter::field(const char* name) {
    if (this->prefix != nullptr && this->prefix[0] != '\0') {
        this->text(this->prefix);
        this->character('.');
    }
    this->text(name);
    this->text(" = ");
}

// --- Registry -----------------------------------------------------------------

void SERIALIZE::expose(World& world, const Id id, const char* name, const SerializeWriteFn write, const SerializeReadFn read, const SerializeInitFn init, const u32 order) {
    if (id == 0 || ECS::IS_PAIR(id) || name == nullptr || !world.alive(id)) {
        return;
    }
    COMPONENT_REGISTRY::preregister<Serializer>("Serializer");
    Serializer serializer;
    strncpy(serializer.name, name, ENTITY_NAME_CAPACITY - 1);
    serializer.write = write;
    serializer.read = read;
    serializer.init = init;
    serializer.order = order;
    world.set(id, serializer);
}

const Serializer* SERIALIZE::of(World& world, const Id id) {
    if (id == 0 || ECS::IS_PAIR(id) || !world.alive(id)) {
        return nullptr;
    }
    return world.get<Serializer>(id);
}

Id SERIALIZE::find(World& world, const char* name, const usz length) {
    if (name == nullptr || length == 0 || length >= ENTITY_NAME_CAPACITY) {
        return 0;
    }
    Id found = 0;
    world.query<Serializer>().each([&](const EntityId entity, Serializer& serializer) {
        if (found == 0 && TEXT_ASSET::word_is(name, length, serializer.name)) {
            found = entity;
        }
    });
    return found;
}

void SERIALIZE::warn(SerializeContext& ctx, const char* format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    char text[640];
    if (ctx.path != nullptr && ctx.line != 0) {
        snprintf(text, sizeof(text), "%s:%u: %s", ctx.path, ctx.line, message);
    } else if (ctx.path != nullptr) {
        snprintf(text, sizeof(text), "%s: %s", ctx.path, message);
    } else {
        snprintf(text, sizeof(text), "%s", message);
    }
    ctx.warnings += 1;
    if (ctx.warn != nullptr) {
        ctx.warn(text, ctx.user_data);
    } else {
        LOG::write_at(LOG_WARNING, LOG_CATEGORY::SCENE, ctx.path, ctx.line, "%s", message);
    }
}

// --- Writers ------------------------------------------------------------------

namespace {

void write_numbers(TextWriter& out, const char* name, const f32* values, const usz count) {
    out.field(name);
    for (usz i = 0; i < count; ++i) {
        if (i > 0) {
            out.character(' ');
        }
        out.number(values[i]);
    }
    out.end_line();
}

} // namespace

void SERIALIZE::write_f32(TextWriter& out, const char* name, const f32 value) {
    write_numbers(out, name, &value, 1);
}

void SERIALIZE::write_i32(TextWriter& out, const char* name, const i32 value) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", value);
    out.field(name);
    out.text(buffer);
    out.end_line();
}

void SERIALIZE::write_u32(TextWriter& out, const char* name, const u32 value) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%u", value);
    out.field(name);
    out.text(buffer);
    out.end_line();
}

void SERIALIZE::write_bool(TextWriter& out, const char* name, const bool value) {
    out.field(name);
    out.text(value ? "true" : "false");
    out.end_line();
}

void SERIALIZE::write_vec2(TextWriter& out, const char* name, const Vector2 value) {
    const f32 values[2] = {value.x, value.y};
    write_numbers(out, name, values, 2);
}

void SERIALIZE::write_vec3(TextWriter& out, const char* name, const Vector3 value) {
    const f32 values[3] = {value.x, value.y, value.z};
    write_numbers(out, name, values, 3);
}

void SERIALIZE::write_vec4(TextWriter& out, const char* name, const Vector4 value) {
    const f32 values[4] = {value.x, value.y, value.z, value.w};
    write_numbers(out, name, values, 4);
}

void SERIALIZE::write_quat(TextWriter& out, const char* name, const Quaternion value) {
    const f32 values[4] = {value.x(), value.y(), value.z(), value.w()};
    write_numbers(out, name, values, 4);
}

void SERIALIZE::write_guid(TextWriter& out, const char* name, const AssetGuid& value) {
    out.field(name);
    if (value.is_null()) {
        out.text("none");
    } else {
        out.guid(value);
    }
    out.end_line();
}

void SERIALIZE::write_word(TextWriter& out, const char* name, const char* word) {
    out.field(name);
    out.text(word != nullptr ? word : "none");
    out.end_line();
}

void SERIALIZE::write_entity_ref(SerializeContext& ctx, TextWriter& out, const char* name, const EntityId entity) {
    out.field(name);
    if (entity == 0 || ctx.world == nullptr || !ctx.world->alive(entity)) {
        out.text("none");
        out.end_line();
        return;
    }
    const EntityIdLow local = ctx.local_id_of != nullptr ? ctx.local_id_of(entity, ctx.scene_data) : 0;
    if (local != 0) {
        char buffer[24];
        snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(local));
        out.text(buffer);
        out.end_line();
        return;
    }
    const AssetUuid* asset = ctx.world->get<AssetUuid>(entity);
    if (asset != nullptr && !asset->guid.is_null()) {
        out.guid(asset->guid);
        out.end_line();
        return;
    }
    out.text("none");
    out.end_line();
    const char* entity_name = SCENE::name(*ctx.world, entity);
    const char* owner_name = SCENE::name(*ctx.world, ctx.entity);
    SERIALIZE::warn(ctx, "%s.%s of '%s' refers to entity %llu ('%s'), which is neither in the scene nor an asset; written as none",
        out.prefix != nullptr ? out.prefix : "", name, owner_name != nullptr ? owner_name : "", static_cast<unsigned long long>(ECS::ENTITY_LOW(entity)),
        entity_name != nullptr ? entity_name : "");
}

// --- Readers ------------------------------------------------------------------

namespace {

bool read_numbers(const char* value, const usz length, f32* out, const usz count) {
    f64 numbers[4];
    usz parsed = 0;
    if (count > 4 || !TEXT_ASSET::parse_numbers(value, length, numbers, count, &parsed) || parsed != count) {
        return false;
    }
    for (usz i = 0; i < count; ++i) {
        out[i] = static_cast<f32>(numbers[i]);
    }
    return true;
}

bool all_digits(const char* value, const usz length) {
    if (length == 0) {
        return false;
    }
    for (usz i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    return true;
}

} // namespace

bool SERIALIZE::read_f32(const char* value, const usz length, f32* out) {
    f32 number = 0.0f;
    if (!read_numbers(value, length, &number, 1)) {
        return false;
    }
    *out = number;
    return true;
}

bool SERIALIZE::read_i32(const char* value, const usz length, i32* out) {
    f64 number = 0.0;
    usz parsed = 0;
    if (!TEXT_ASSET::parse_numbers(value, length, &number, 1, &parsed) || parsed != 1 || number != static_cast<f64>(static_cast<i32>(number))) {
        return false;
    }
    *out = static_cast<i32>(number);
    return true;
}

bool SERIALIZE::read_u32(const char* value, const usz length, u32* out) {
    f64 number = 0.0;
    usz parsed = 0;
    if (!TEXT_ASSET::parse_numbers(value, length, &number, 1, &parsed) || parsed != 1 || number < 0.0 || number != static_cast<f64>(static_cast<u32>(number))) {
        return false;
    }
    *out = static_cast<u32>(number);
    return true;
}

bool SERIALIZE::read_bool(const char* value, const usz length, bool* out) {
    if (TEXT_ASSET::word_is(value, length, "true") || TEXT_ASSET::word_is(value, length, "1")) {
        *out = true;
        return true;
    }
    if (TEXT_ASSET::word_is(value, length, "false") || TEXT_ASSET::word_is(value, length, "0")) {
        *out = false;
        return true;
    }
    return false;
}

bool SERIALIZE::read_vec2(const char* value, const usz length, Vector2* out) {
    f32 numbers[2];
    if (!read_numbers(value, length, numbers, 2)) {
        return false;
    }
    *out = Vector2(numbers[0], numbers[1]);
    return true;
}

bool SERIALIZE::read_vec3(const char* value, const usz length, Vector3* out) {
    f32 numbers[3];
    if (!read_numbers(value, length, numbers, 3)) {
        return false;
    }
    *out = Vector3(numbers[0], numbers[1], numbers[2]);
    return true;
}

bool SERIALIZE::read_vec4(const char* value, const usz length, Vector4* out) {
    f32 numbers[4];
    if (!read_numbers(value, length, numbers, 4)) {
        return false;
    }
    *out = Vector4(numbers[0], numbers[1], numbers[2], numbers[3]);
    return true;
}

bool SERIALIZE::read_quat(const char* value, const usz length, Quaternion* out) {
    f32 numbers[4];
    if (!read_numbers(value, length, numbers, 4)) {
        return false;
    }
    *out = Quaternion(numbers[0], numbers[1], numbers[2], numbers[3]);
    return true;
}

bool SERIALIZE::read_guid(const char* value, const usz length, AssetGuid* out) {
    if (TEXT_ASSET::word_is(value, length, "none")) {
        *out = AssetGuid{};
        return true;
    }
    return TEXT_ASSET::parse_guid(value, length, out);
}

bool SERIALIZE::read_entity_ref(SerializeContext& ctx, const char* value, const usz length, EntityId* out) {
    if (TEXT_ASSET::word_is(value, length, "none")) {
        *out = 0;
        return true;
    }
    // A GUID is always 32 hex digits, so a shorter run of digits is a local id.
    if (length < TEXT_ASSET::GUID_TEXT_LENGTH && all_digits(value, length)) {
        char buffer[32];
        if (!TEXT_ASSET::copy_span(value, length, buffer, sizeof(buffer))) {
            return false;
        }
        const EntityIdLow local = static_cast<EntityIdLow>(strtoull(buffer, nullptr, 10));
        const EntityId entity = ctx.entity_of_local_id != nullptr && local != 0 ? ctx.entity_of_local_id(local, ctx.scene_data) : 0;
        if (entity == 0) {
            SERIALIZE::warn(ctx, "no entity %llu in this file; the reference is left empty", static_cast<unsigned long long>(local));
        }
        *out = entity;
        return true;
    }
    AssetGuid guid;
    if (!TEXT_ASSET::parse_guid(value, length, &guid)) {
        return false;
    }
    EntityId entity = 0;
    if (ctx.resolve_asset != nullptr) {
        entity = ctx.resolve_asset(guid, ctx.user_data);
    } else if (ctx.world != nullptr) {
        entity = ASSET_ENTITY::find(*ctx.world, guid);
    }
    if (entity == 0) {
        char text[TEXT_ASSET::GUID_TEXT_CAPACITY];
        TEXT_ASSET::format_guid(guid, text);
        SERIALIZE::warn(ctx, "asset %s is not registered; the reference is left empty", text);
    }
    *out = entity;
    return true;
}
