#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/world.hpp"
#include "engine/math/math.hpp"
#include "engine/scene/scene.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cstring>
#include <type_traits>

// How a component is written to and read from a text asset (a .scene file,
// later prefabs and Luau access). Like the Inspector (inspector.hpp), the
// information is a component on the component entity itself: a Serializer
// set by SERIALIZE::expose<T>() names the component in the file and holds
// the two functions that turn its bytes into `Component.field = value`
// lines and back. A tag is exposed with expose_tag<T>(): it has no fields
// and is listed in an entity's `tags = ...` line by its name.
//
//     SERIALIZE::expose<Transform>(world, "Transform", &transform_write, &transform_read, 0);
//     SERIALIZE::expose_tag<RenderCamera>(world, "RenderCamera");
//
// A write function emits every field through the helpers below:
//
//     static void transform_write(SerializeContext& ctx, const void* data, TextWriter& out) {
//         const Transform* transform = static_cast<const Transform*>(data);
//         SERIALIZE::write_vec3(out, "position", transform->position);
//         SERIALIZE::write_quat(out, "rotation", transform->rotation);
//     }
//
// and a read function is called once per `Component.field` line with the
// part after the dot and the text after `=`; it returns false for a field
// it does not know or a value that does not parse, and the reader then
// warns with the file and line and skips the line. A field that is not in
// the file keeps the component's default (Serializer::init writes it).
//
// Entity references never go through ad hoc code: write_entity_ref /
// read_entity_ref implement the one rule (a file-local id for an entity of
// the scene being written, the asset GUID for an entity carrying AssetUuid,
// `none` otherwise). SerializeContext carries what those need: the scene
// writer / reader fill the local-id callbacks, the Engine the asset
// resolver and the warning sink. Core knows nothing about what loading an
// asset means; the resolver does.

struct SerializeContext;

// Appends text to a DynamicArray<char>; what write functions get. `prefix`
// is the serializer's name, so field("position") emits
// "Transform.position = ".
struct TextWriter {
    DynamicArray<char>* out = nullptr;
    const char* prefix = nullptr;

    void text(const char* text);
    void span(const char* text, usz length);
    void character(char c);
    // The fewest significant digits that read back to the same f32.
    void number(f64 value);
    void guid(const AssetGuid& guid);
    // "<prefix>.<name> = " (or "<name> = " without a prefix).
    void field(const char* name);
    void end_line() { this->character('\n'); }
};

// Writes every field of `data` as "<prefix>.<field> = value" lines.
using SerializeWriteFn = void (*)(SerializeContext& ctx, const void* data, TextWriter& out);
// One field: `field` is the part after the dot, `value` the text after '='.
// Returns false when the field is unknown or the value does not parse.
using SerializeReadFn = bool (*)(SerializeContext& ctx, void* data, const char* field, usz field_length, const char* value, usz value_length);
// Writes the default value of the component into `data`, which holds zeroed
// bytes of its size: what a component gets before its fields are read.
using SerializeInitFn = void (*)(void* data);

struct Serializer {
    // The "Component" in Component.field; a tag's word in `tags`.
    char name[ENTITY_NAME_CAPACITY] = {};
    SerializeWriteFn write = nullptr; // nullptr for tags
    SerializeReadFn read = nullptr;   // nullptr for tags
    SerializeInitFn init = nullptr;   // nullptr leaves the zeroed bytes
    // Components are written lowest order first, then by id.
    u32 order = 100;

    bool is_tag() const { return this->write == nullptr; }
};

// An asset GUID -> the entity to reference (loading it if that is what the
// asset's type needs), 0 when unknown.
using SerializeResolveFn = EntityId (*)(const AssetGuid& guid, void* user_data);

struct SerializeContext {
    World* world = nullptr;
    // The entity being written / read.
    EntityId entity = 0;

    // Filled by the scene writer / reader (SCENE_ASSET), never by the caller.
    // Writing: the file-local id of an in-scene entity, 0 if not in the scene.
    EntityIdLow (*local_id_of)(EntityId entity, void* scene_data) = nullptr;
    // Reading: the entity created for a file-local id, 0 if unknown.
    EntityId (*entity_of_local_id)(EntityIdLow id, void* scene_data) = nullptr;
    void* scene_data = nullptr;

    // Reading: an asset GUID -> entity. Without one, read_entity_ref falls
    // back to ASSET_ENTITY::find, which never loads anything.
    SerializeResolveFn resolve_asset = nullptr;
    void* user_data = nullptr;

    // Diagnostics: the file and the line being read / the entity being
    // written, for warnings; `warn` receives the formatted text prefixed
    // with `path:line: ` (a LOG_CATEGORY::SCENE warning carrying the
    // location when nullptr). `warnings` counts them.
    const char* path = nullptr;
    u32 line = 0;
    void (*warn)(const char* text, void* user_data) = nullptr;
    usz warnings = 0;
};

namespace SERIALIZE {

// Sets the Serializer of component `id` (a component entity, not a pair).
// Calling it again replaces the previous one. No-op for 0, pairs, dead ids
// or a nullptr name. `write` and `read` both null makes a tag serializer.
void expose(World& world, Id id, const char* name, SerializeWriteFn write, SerializeReadFn read, SerializeInitFn init = nullptr, u32 order = 100);

// The init a value-initialized T gives: `T value {}` copied into the data.
template <typename T>
void default_init(void* data) {
    const T value {};
    memcpy(data, &value, sizeof(T));
}

template <typename T>
void expose(World& world, const char* name, const SerializeWriteFn write, const SerializeReadFn read, const u32 order = 100) {
    static_assert(!std::is_empty_v<T>, "expose<T>: use expose_tag<T> for a tag");
    SERIALIZE::expose(world, world.component<T>(), name, write, read, &SERIALIZE::default_init<T>, order);
}
template <typename T>
void expose_tag(World& world, const char* name) {
    static_assert(std::is_empty_v<T>, "expose_tag<T>: T has data, use expose<T>");
    SERIALIZE::expose(world, world.tag<T>(), name, nullptr, nullptr, nullptr, 100);
}

// The Serializer of component `id`, or nullptr if it has none, or `id` is a
// pair, 0, or not alive. Invalidated by structural changes on the
// component entity.
const Serializer* of(World& world, Id id);
// The component whose Serializer is named `name` (`length` characters), or
// 0 when none is. A scan over the serializers; fine per line for now.
Id find(World& world, const char* name, usz length);

// Reports a warning through ctx.warn (or stderr), prefixed with
// "path:line: " when the context has a path, and counts it.
void warn(SerializeContext& ctx, const char* format, ...);

// --- Field helpers -----------------------------------------------------------
//
// Writers emit one "<prefix>.<name> = value" line. Readers take the value
// text and return false when it does not parse, leaving `out` untouched.

void write_f32(TextWriter& out, const char* name, f32 value);
void write_i32(TextWriter& out, const char* name, i32 value);
void write_u32(TextWriter& out, const char* name, u32 value);
void write_bool(TextWriter& out, const char* name, bool value);
void write_vec2(TextWriter& out, const char* name, Vector2 value);
void write_vec3(TextWriter& out, const char* name, Vector3 value);
void write_vec4(TextWriter& out, const char* name, Vector4 value);
// x y z w.
void write_quat(TextWriter& out, const char* name, Quaternion value);
// 32 hex digits, or `none` for a null GUID.
void write_guid(TextWriter& out, const char* name, const AssetGuid& value);
// A bare word (an enum's name).
void write_word(TextWriter& out, const char* name, const char* word);
// An entity reference by the rule above; `name` is also what the warning
// for an entity that cannot be referenced says.
void write_entity_ref(SerializeContext& ctx, TextWriter& out, const char* name, EntityId entity);

bool read_f32(const char* value, usz length, f32* out);
bool read_i32(const char* value, usz length, i32* out);
bool read_u32(const char* value, usz length, u32* out);
// `true` / `false` (also 1 / 0).
bool read_bool(const char* value, usz length, bool* out);
bool read_vec2(const char* value, usz length, Vector2* out);
bool read_vec3(const char* value, usz length, Vector3* out);
bool read_vec4(const char* value, usz length, Vector4* out);
bool read_quat(const char* value, usz length, Quaternion* out);
// 32 hex digits or `none` (the null GUID).
bool read_guid(const char* value, usz length, AssetGuid* out);
// A number (fewer than 32 digits) is a file-local id looked up through the
// context, 32 hex digits are a GUID that goes through the resolver, `none`
// is 0. An unknown local id or GUID warns and
// gives 0; only a value that is none of these returns false.
bool read_entity_ref(SerializeContext& ctx, const char* value, usz length, EntityId* out);

} // namespace SERIALIZE
