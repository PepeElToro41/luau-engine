#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/scene/serialize.hpp"
#include "engine/templates/dynamic_array.hpp"

// A scene asset (ASSET_TYPE::SCENE): a `.scene` text file holding a tree of
// entities, every one of them with its Name, its parent and the components
// a Serializer (scene/serialize.hpp) knows how to write. docs/asset_format.md
// "Scene files" is the specification.
//
//     guid = 3f1c9a0e7b2d4c6f8a1b2c3d4e5f6a7b
//
//     [entity 1]
//     name = camera
//     Transform.position = 0 1.5 4.5
//     Transform.rotation = -0.1650476 0 0 0.9862856
//     Transform.scale = 1 1 1
//     Camera.fov_y = 1.0471976
//     tags = RenderCamera
//
//     [entity 2]
//     name = wheel
//     parent = 1
//     PrimitiveRenderer.shape = cube
//     PrimitiveRenderer.material = 1786f887a24190b435cd7dd4cbef587f
//
// The number of an `[entity N]` is file-local: regenerated at every save in
// depth-first tree order, meaningful only to `parent = N` and the entity
// references inside the file. The loader never reuses it as an EntityId:
// every section gets a fresh new_entity() and a table maps the numbers to
// them, which is what makes instantiating a file under any entity (a
// prefab) the same operation as loading it under scene_root. Entities
// outside the scene are referenced by asset GUID or not at all (see
// SERIALIZE::write_entity_ref); a file-backed entity such as a material is
// never embedded.
//
// Reading is forgiving line by line: an unknown component, an unknown field
// or a bad value warns with `path:line` and skips that line, and a missing
// field keeps the component's default. Only a structural problem (no guid,
// a duplicate or malformed `[entity N]`, a malformed line) fails the read,
// and then nothing was created.

namespace SCENE_ASSET {

constexpr const char* EXTENSION = ".scene";

// Appends `guid` and every descendant of `root` (not `root` itself) in
// depth-first order (children lowest id first, as the Explorer lists them)
// to `out`, no terminator, in the canonical layout: `name`, `parent`, then
// the components with a Serializer lowest `order` first, each field as the
// serializer writes it, then `tags`. Entities with a reference outside the
// scene are written per the reference rules; warnings go to ctx.warn.
// `ctx` is the caller's (resolver, warning sink); the writer fills its
// local-id callbacks while it runs.
void write(World& world, EntityId root, const AssetGuid& guid, SerializeContext& ctx, DynamicArray<char>& out);
// write() to the file at `path`, replacing it. False (with a message) when
// the guid is null or the file cannot be written.
bool write_file(World& world, EntityId root, const AssetGuid& guid, SerializeContext& ctx, const char* path);

struct SceneReadResult {
    // Entities created.
    usz entities = 0;
    // Lines skipped with a warning.
    usz warnings = 0;
    // False for a structural failure: `error` says what and `line` where
    // (0 when it is not a line), and nothing was created.
    bool ok = false;
    const char* error = nullptr;
    u32 line = 0;
    // The file's guid.
    AssetGuid guid;
};

// Instantiates the file's entities under `parent` (which must be alive).
// Two passes: every `[entity N]` becomes new_entity() with its Name and its
// CHILD_OF (the section's `parent`, or `parent` when it has none), then the
// components and tags are read, so a parent or a reference may appear
// after the section that names it. `ctx.path` is used in warnings.
SceneReadResult read(World& world, const char* text, usz size, EntityId parent, SerializeContext& ctx);

} // namespace SCENE_ASSET
