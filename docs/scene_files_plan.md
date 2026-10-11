# Scene files: plan

Status: implemented 2026-10-11; `docs/asset_format.md` "Scene files" is
the specification now and this file is the design record. Written
2026-10-10 for whoever implements it. The decisions below were discussed and settled; the API
sketches are a starting point and may change where the code says otherwise,
but the format rules and the "why not" sections should hold. Record the
final layout in `docs/asset_format.md` once it exists, as that file is the
specification for every asset format.

Read `CLAUDE.md` and the module files it imports first: they describe the
ECS, the scene hierarchy (`engine/core/include/engine/scene/scene.hpp`), the
text asset syntax (`engine/core/include/engine/asset/text_asset.hpp`), the
asset entities (`engine/core/include/engine/asset/asset_entity.hpp`), the
render components (`engine/graphics/include/engine/render/components.hpp`)
and the editor. Follow the C++ conventions there (`struct` only, `this->`,
`snake_case`, no destructors, `SCREAMING_CASE` namespaces, doctest tests
under `tests/core/`).

## Goal

The engine can draw a scene but not persist one: both apps spawn the
hard-coded `RENDER_DEMO` scene and the editor's work dies on exit. This
adds:

- a `.scene` text asset that stores everything under `scene_root`,
- saving the current scene and loading another one (unload + instantiate),
- instantiating a scene file under any entity, which is what a prefab is,
- the component reflection layer (`Serializer`) that the scene file needs
  and that Luau access and prefabs will reuse later.

Out of scope for this round: dirty tracking of the scene with undo, prefab
overrides, scene-local embedded assets, world-matrix propagation, async or
time-sliced loading.

## What already exists and is relied on

- `Scene` singleton with `root` (`scene_root`), `SCENE::children` (direct
  children, lowest id first, stable frame to frame), `SCENE::spawn`,
  `SCENE::set_parent`, `Name`.
- Deleting an entity deletes its subtree (`CHILD_OF` cleanup policy).
  Measured in Release: ~28 ns per cascaded child, ~167 ns to create and
  delete an entity with 4 components. A 100k-entity scene unloads in well
  under 50 ms. Unloading is therefore synchronous on the main thread.
- Text assets: `TextAssetCursor` walks `key = value` lines and `[section]`
  headers; `TEXT_ASSET::parse_guid` / `format_guid` / `parse_numbers` /
  `next_word`; `TEXT_ASSET::read_prelude(path)` synthesizes the prelude the
  provider keys on; `type_of_path` / `extension_of_type` map extensions to
  `ASSET_TYPE::*`. `ASSET_TYPE::SCENE` (`"SCNE"`) and the `AssetScene` tag
  already exist; only the `.scene` extension has to be added.
- `MATERIAL_ASSET::write(material, DynamicArray<char>&)` and `write_file`
  show the canonical-text writing style (shortest round-trip floats).
- Asset entities: every registered file is an entity with `AssetUuid`,
  `(AssetType, tag)`, `Name`, and `AssetPath` when the registrar knew the
  file. `ASSET_ENTITY::find(world, guid)`; `AssetEntities` singleton on the
  Engine. A `.material` loads onto its own asset entity (`MATERIAL::load`).
- Dirty tracking for file-backed assets is done (commit `b01fc18`): the
  `AssetDirty` tag, an observer on `Material` that sets it on CHANGED,
  `MATERIAL::load` / `reload` / `save` clear it, the pathless
  `MATERIAL::save(renderer, entity)`, and the editor's File > Save All
  (Ctrl+S, `App::save_all`) over every tagged material. The scene joins
  that action.
- The `Inspector` / `Addable` components live on the component entity
  itself (`engine/core/include/engine/scene/inspector.hpp`,
  `INSPECTOR::expose<T>` / `of` / `addable<T>`). The `Serializer` follows
  the same pattern exactly.

## Decisions

### Entity identifiers in the file

Ids in a scene file are **file-local, regenerated at every save**,
assigned in depth-first tree order (children lowest entity id first, the
order the Explorer shows). They exist only so references inside the file
can be expressed; today that is just `parent`.

Why not the raw `EntityId`: it is only unique within one run of one World.
Materials, shaders and asset entities take ids before any scene loads, so
file ids collide with live ones the moment a second scene loads, and the
loader would have to remap anyway.

Why not a stored per-entity uuid: nothing needs stability across saves
yet (no prefab overrides, no cross-scene references). The syntax leaves
room for it: if it is ever needed, the number in the section header
becomes stored instead of regenerated and nothing else changes.

Hard rule for the loader: **never `make_alive` a file id.** Every section
gets a fresh `new_entity()` and a file-id to entity table resolves
references. This is also what makes additive loading (prefabs) free.

### Entity references

Every `EntityId` field a component serializes goes through one pair of
helpers, `write_entity_ref` / `read_entity_ref`, never through ad hoc code.
Writing decides by what the target is:

| target | written as |
|---|---|
| an entity inside the scene being saved | its file-local id, a plain number |
| an entity carrying `AssetUuid` | the asset GUID, 32 hex digits |
| anything else | `none`, plus a warning naming the entity and the field |

Reading mirrors it: a number is looked up in the file-id table (filled in
pass one, so forward references work); a GUID goes through the resolver
the Engine provides (see below); `none` is 0; an unknown GUID resolves to
0 with a warning and the field is left for the user to fix in the
Inspector, whose combos already show an unknown value as such.

Asset references that are already GUIDs in the component
(`MeshRenderer::mesh`) are written as GUIDs directly.

File-backed entities (materials) are **never embedded** in the scene file:
two copies of one material means picking a winner on load and silently
losing an edit, and a material shared by three scenes would drift three
ways. The dirty-tag Save All guarantees the referenced file is current when
the scene is written. If a scene-local sub-asset is ever wanted, an
`[embedded N]` section outside the entity list is the reserved shape; it
would not touch the reference syntax.

### Loading and unloading

Synchronous, on the main thread. `instantiate(file, parent)` is the
primitive: load a scene = delete the children of `scene_root` and
instantiate under it; instantiate under any other entity = prefab. GPU
resources are keyed by GUID and shared, so a scene switch does not touch
the GPU asset cache (a release-unused pass is a later concern).

If a loading screen is ever wanted, build under a detached holder entity
and reparent its top-level children at the end (one `set_parent` each);
that is where time-slicing would plug in. Not now.

## File format

Extension `.scene`, UTF-8, the flat `key = value` / `[section]` syntax of
`text_asset.hpp`. Comments and blank lines as the cursor already allows.

```
# optional comment
guid = 3f1c9a0e7b2d4c6f8a1b2c3d4e5f6a7b

[entity 1]
name = camera
Transform.position = 0 1.5 4.5
Transform.rotation = -0.1650476 0 0 0.9862856
Transform.scale = 1 1 1
Camera.fov_y = 1.0471976
Camera.near_plane = 0.1
Camera.far_plane = 1000
tags = RenderCamera

[entity 2]
name = cube
Transform.position = 0 0 0
Transform.rotation = 0 0.258819 0 0.9659258
Transform.scale = 1 1 1
PrimitiveRenderer.shape = cube
PrimitiveRenderer.material = 1786f887a24190b435cd7dd4cbef587f

[entity 3]
name = wheel
parent = 2
Transform.position = 0.5 0 0
MeshRenderer.mesh = 9c2d1e0f3a4b5c6d7e8f9a0b1c2d3e4f
MeshRenderer.material = none
```

Rules:

- Top level: `guid` (required, 32 hex digits, `hi` then `lo`, as every GUID
  prints). Nothing else top level for now.
- `[entity N]`: N is the file-local id, a positive integer, unique in the
  file. Sections appear in depth-first order on save; the loader must not
  depend on order beyond "a parent may appear after its child" being legal
  (pass one creates every entity before any reference is resolved).
- `name = <text>`: the `Name` component, raw text to end of line, cut to
  `ENTITY_NAME_CAPACITY`. Omitted when empty.
- `parent = N`: file-local id of the parent. Absent means the instantiate
  target (`scene_root` for a scene load). `parent = 0` is the same as
  absent.
- `tags = A B C`: data-less components by serializer name, space separated.
  Written only when there is at least one.
- `Component.field = <value>`: one line per field, dispatched on the part
  before the first dot to the `Serializer` registered under that name.
  Values follow the existing conventions: numbers as the shortest
  round-trip text, vectors as space-separated numbers, quaternions as
  `x y z w`, booleans as `true` / `false`, enums as words, GUIDs as hex,
  entity references as described above.
- Rotation is saved as the quaternion so a round trip is lossless.
- Unknown component name, unknown field, bad value: warn with
  `path:line` and skip that line; the rest of the file still loads. A
  missing field keeps the component's default. A component with no
  serializer is not saved at all (silently; editor-only components such
  as `EditorCamera` never sit under `scene_root` anyway).
- Writer: `Name` first, then `parent`, then components ordered by their
  serializer's `order` then id (same idea as `Inspector::order`), fields
  in the order the serializer writes them, then `tags`. Canonical output
  so a save after a load is byte-identical when nothing changed.

## The `Serializer` layer (core)

`engine/core/include/engine/scene/serialize.hpp` (+ `src/scene/serialize.cpp`),
mirroring `inspector.hpp`:

```cpp
struct SerializeContext;   // defined in core, below
struct TextWriter;         // small text builder on a BaseAllocator (DynamicArray<char>)

// Writes every field of `data` as "<prefix>.<field> = value" lines.
using SerializeWriteFn = void (*)(SerializeContext& ctx, const void* data, TextWriter& out);
// One field: `field` is the part after the dot, `value` the text after '='.
// Returns false when the field is unknown or the value does not parse.
using SerializeReadFn = bool (*)(SerializeContext& ctx, void* data, const char* field, usz field_length, const char* value, usz value_length);

struct Serializer {
    char name[ENTITY_NAME_CAPACITY] = {};   // the "Component" in Component.field
    SerializeWriteFn write = nullptr;       // nullptr for tags
    SerializeReadFn read = nullptr;         // nullptr for tags
    u32 order = 100;
};

namespace SERIALIZE {
void expose(World& world, Id id, const char* name, SerializeWriteFn write, SerializeReadFn read, u32 order = 100);
template <typename T> void expose(World& world, const char* name, SerializeWriteFn write, SerializeReadFn read, u32 order = 100);
template <typename T> void expose_tag(World& world, const char* name);   // write/read null
const Serializer* of(World& world, Id id);          // nullptr for pairs and unexposed ids
Id find(World& world, const char* name, usz length); // 0 when no serializer has that name
}
```

Name lookup during load is per line; keep it cheap (a scan over the
component entities holding `Serializer` is fine for a first version, a
`HashMap` by name hash on the World side later if it shows).

`SerializeContext` carries what field helpers need and keeps core free of
graphics:

```cpp
using SerializeResolveFn = EntityId (*)(const AssetGuid& guid, void* user_data);

struct SerializeContext {
    World* world = nullptr;
    EntityId entity = 0;                 // the entity being written / read
    // Writing: the file-local id of an in-scene entity, 0 if not in the scene.
    EntityIdLow (*local_id_of)(EntityId entity, void* user_data) = nullptr;
    // Reading: the entity created for a file-local id, 0 if unknown.
    EntityId (*entity_of_local_id)(EntityIdLow id, void* user_data) = nullptr;
    // Reading: an asset GUID -> entity (loads it if that is what the type needs).
    SerializeResolveFn resolve_asset = nullptr;
    void* user_data = nullptr;
    // Diagnostics: path and current line for warnings, and a log sink.
    const char* path = nullptr;
    u32 line = 0;
    void (*warn)(const char* text, void* user_data) = nullptr;
};
```

Field helpers in `SERIALIZE::` (header-only or in the .cpp): `write_f32`,
`write_vec3`, `write_vec4`, `write_quat`, `write_bool`, `write_guid`,
`write_word`, `write_entity_ref`, and the matching `read_*` taking
`(value, length, out)`; `read_entity_ref(ctx, value, length, EntityId* out)`
implements the reference rules above. Reuse `TEXT_ASSET::parse_numbers`,
`parse_guid`, `format_guid`, `next_word`; use the same float formatting as
`MATERIAL_ASSET::write` (factor it out if it is private there).

Tests (`tests/core/scene/serialize_tests.cpp`): expose the sample
`Position` / `Velocity` / tags from `test_support.hpp`, check `of` / `find`,
each field helper round-trips, `read_entity_ref` with a local id, a GUID
(a stub resolver), `none`, and an unknown GUID.

## The scene asset (core)

`engine/core/include/engine/asset/asset_types/scene_asset.hpp`
(+ `src/asset/asset_types/scene_asset.cpp`):

```cpp
namespace SCENE_ASSET {
constexpr const char* EXTENSION = ".scene";

// Writes `guid` and every descendant of `root` (not `root` itself) in
// depth-first order into `out`. Entities with a reference outside the
// scene are written per the reference rules; warnings go to ctx.warn.
void write(World& world, EntityId root, const AssetGuid& guid, SerializeContext& ctx, DynamicArray<char>& out);
bool write_file(World& world, EntityId root, const AssetGuid& guid, SerializeContext& ctx, const char* path);

struct SceneReadResult { usz entities = 0; usz warnings = 0; bool ok = false; AssetGuid guid; };

// Instantiates the file's entities under `parent`. Two passes: every
// [entity N] becomes new_entity() + Name + CHILD_OF (parent or `parent`),
// then components and tags are read. Line-level problems warn and skip;
// `ok` is false only for a structural failure (no guid, duplicate id,
// unreadable text), in which case nothing was created.
SceneReadResult read(World& world, const char* text, usz size, EntityId parent, SerializeContext& ctx);
}
```

Add `.scene` to `TEXT_ASSET::type_of_path` / `extension_of_type` so
`ASSET_FILE::read_prelude_any` registers it, `Engine::load_asset_file`
gives it an asset entity typed `AssetScene`, and the provider serves its
text through the `TEXT` chunk like a `.material`.

Pass one must create all entities before any `parent` is applied, since a
parent may be referenced before its section. Use the test components for
round-trip tests (`tests/core/asset/scene_asset_tests.cpp`): a 3-level tree
round-trips with identical text; names; a parent referenced before it is
defined; a tag; an unknown component warns and skips; a GUID reference
resolved through a stub resolver; `write` of a root with no children
yields just the guid line; `read` into a non-root parent puts every
top-level entity under it (the prefab case). Keep `World` explicit
(`init` / `free`, `CHECK_ARENA_CLEAN()`), no fixtures.

## Scene operations (core `Scene`)

On `Scene` (`scene.hpp`):

```cpp
// Deletes every child of root (and so every descendant). Root survives.
void unload();
// The scene file the content under root came from, null for a new scene,
// and its path; set by whoever loads / saves (the Engine).
AssetGuid current;
char current_path[ASSET_PATH_CAPACITY];
```

`instantiate` itself is `SCENE_ASSET::read(world, text, size, parent, ctx)`;
`Scene` does not read files, the Engine does (it owns the provider and the
resolver).

## Graphics: serializers for the render components

In `RENDER_COMPONENTS::register_all` (`engine/graphics/src/render/components.cpp`),
next to the `Inspector` registrations the editor does, expose:

- `Transform`: `position` (vec3), `rotation` (quat `x y z w`), `scale` (vec3).
- `Camera`: `fov_y` (radians, as stored), `near_plane`, `far_plane`.
- `RenderCamera`: tag.
- `MeshRenderer`: `mesh` (GUID), `material` (entity ref).
- `PrimitiveRenderer`: `shape` (word: `cube` / `sphere` / `cylinder`, via
  `PRIMITIVES::name` and a reverse lookup; never the enum number),
  `material` (entity ref).

`Shader` and `Material` get no serializer: they are not scene content and
are referenced by GUID.

## Runtime (`Engine`)

```cpp
// Loads the scene asset `guid` (registered through load_asset_file) into
// scene_root after unloading what is there. False (with messages) when the
// asset is unknown, not a .scene, or structurally broken; the old content
// is then still gone only if the text was readable - unload after the
// text is in hand, not before.
bool load_scene(const AssetGuid& guid);
// Instantiates the scene asset under `parent` without unloading anything.
bool instantiate_scene(const AssetGuid& guid, EntityId parent);
// Writes the content under scene_root to `path` with Scene::current as
// guid (a fresh random GUID when null), registers the file if it is new,
// and sets current / current_path.
bool save_scene(const char* path);
// Unloads and clears current.
void new_scene();
```

The Engine builds the `SerializeContext`: `resolve_asset` finds the asset
entity with `AssetEntities::find`, reads its `AssetType`, and for
`MATERIAL` calls `MATERIAL::load`, for `SHADER` `SHADER_LIBRARY::load`,
otherwise returns the asset entity. Warnings go to the renderer log sink
(`RENDERER::log` at warning level) so the editor's Output panel shows
them; the standalone prints them.

Random GUIDs: the editor has `IMPORT::random_guid`; the runtime needs its
own (or move it to core `utils/`), since the standalone may save too.

## Apps

- Write the demo scene out once as `editor/testing_project/scenes/demo.scene`
  (camera + cube, sphere, cylinder, using `materials/demo.material`) and
  make both apps load a scene file at startup instead of calling
  `RENDER_DEMO::spawn`. Keep `RENDER_DEMO` only for the post / shadow /
  texture toggles, or drop what becomes dead. Note the demo's code-built
  orange material cannot be saved (no file); the saved scene references
  `demo.material` instead.
- Editor: File > New Scene / Open Scene... / Save Scene / Save Scene As...,
  Create > Scene in the Asset Browser (a `.scene` with a fresh GUID and no
  entities), double-click a `.scene` in the Asset Browser opens it, and
  `App::save_all` saves the scene after the dirty materials. The window or
  Explorer header shows `current_path`. Before New / Open / Quit, if
  anything is dirty, a modal with Save / Discard / Cancel. Scene dirty
  flag: set at the editor's mutation sites (Inspector `modified`, Explorer
  New Entity, delete, Add Component, reparent), cleared on save; at quit,
  serialize to a buffer and compare with the hash recorded at the last
  save as the safety net.
- Standalone: `--scene <path>` argument, defaulting to the project's
  `scenes/demo.scene`.

## Work order

1. Core `Serializer` registry, context, writer, field helpers, tests.
2. Core `SCENE_ASSET::write` / `read`, `.scene` extension, tests.
3. `Scene::unload` / `current`; `Engine::load_scene` / `instantiate_scene`
   / `save_scene` / `new_scene` with the resolver.
4. Graphics serializers in `register_all`.
5. `demo.scene` written, both apps load it; editor menu, Create > Scene,
   double-click, Save All includes the scene, dirty prompts.
6. Update `engine/core/CLAUDE.md`, `engine/graphics/CLAUDE.md`,
   `engine/runtime/CLAUDE.md`, `editor/CLAUDE.md` (one clause per line, see
   the "Editing these docs" rules) and record the format in
   `docs/asset_format.md`.

Steps 1 and 2 build and test with `engine_tests` alone (core only, no
SDL / Vulkan). Commit per step.

## Known issue to be aware of

Both apps log `material of unlit has no block member 'color'` at startup
(from `RENDER_DEMO::spawn`'s `set_vec4(orange, "color")`), although
`render/unlit.slang` declares `color`. This predates the dirty-tag work and
is unrelated to scenes, but it means the code-built demo material draws
with a zeroed block. Worth fixing or at least understanding before scene
files start saving material values; it is probably in the material
interface / reflection path (`SHADER_PROGRAM::load`, `MATERIAL::sync`).
