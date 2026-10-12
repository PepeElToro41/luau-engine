# engine/core

`engine/core` -> `LuauEngine::core`: `defines.hpp`, `memory/`, `templates/`, `utils/`, `math/`, `ecs/`, `scene/`, `asset/`, `geometry/`, `platform/`, `scheduler/`.
No external dependencies.
Keep it that way: tests and benchmarks link only this.
`memory/` is the allocators: `BaseAllocator` (the interface `DynamicArray` and the containers take), the heap, `ArenaAllocator` (bump, `reset` / `reset_to`, no `reallocate`),
`TemporalAllocator` (scoped scratch on the thread's `MAIN_ARENA`, rewound when it goes out of scope;
`Query::each` holds one while it runs, so a callback must not grow a container that lives on an outer one;
an arena cannot grow a block and its `free` is a no-op, so a container on a temporal allocator or any arena is `reserve`d to its final size or an upper bound such as a `SparseList`'s `alive_count` before it is filled, otherwise every growth leaves the old block dead until the rewind) and `FrameAllocator` (an arena for one frame, `reset` once per frame by its owner, the right home for anything a callback fills that the rest of the frame reads).
`math/` is header-only (`math/math.hpp` includes it all): `Vector2` (scalar, 8 bytes),
`Vector3` (SIMD, 16 bytes with a padding lane `w` that nothing reads;
`load`/`store` move 12-byte vertex data),
`Vector4`, `Quaternion` (a `Vector4` x, y, z, w with w scalar;
`a * b` applies b first),
`Matrix4x4` (four `Vector4` columns, column-major GLSL layout, column vectors).
`math/simd.hpp` is the register layer, `SIMD::f32x4` with SSE2 on x86-64 and a scalar fallback elsewhere or with `ENGINE_MATH_FORCE_SCALAR` defined (verify both: `cmake -S . -B build-scalar -DCMAKE_CXX_FLAGS=-DENGINE_MATH_FORCE_SCALAR`).
Binary helpers are free functions in `MATH::` (`dot`, `cross`, `lerp`, `min`, `max`, `approx_equal`, `slerp`, ...).
Conventions: right-handed, +Y up, forward -Z, radians;
`Matrix4x4::perspective` / `orthographic` produce Vulkan clip space (depth 0..1, Y negated) for a LESS depth test with depth cleared to 1.0;
`look_at` is the world-to-view matrix.
`platform/` is the OS layer: one public header per area (`platform/file.hpp`: `File` + `PLATFORM::file_open/close/size/read/write`, positional and 64-bit) implemented once per OS in `src/platform/linux/` and `src/platform/windows/`;
every source there is wrapped in its platform macro since sources are globbed.
Nothing else in core calls the OS directly;
the asset reader and writer go through it.
`asset/asset_view.hpp` is the `.lunaasset` byte layout (header, dependency table, chunk table, aligned payloads;
`CHUNK_TYPE::*` tags) plus `AssetView`, a view over the prelude only: `AssetView::parse(data, size)` returns a view whose `is_ok()` / `parse_error` say how it went.
`asset/asset_writer.hpp` is `AssetWriter` (collects dependencies and chunk payloads, `write()` emits the file) and `ASSET_FILE::write_file`.
Neither knows about chunk contents.
Nothing loads a whole asset file: `asset/asset_reader.hpp` (`AssetReader`) opens the file, keeps its header, and reads one chunk at a time by seeking with the view's chunk table (`read_chunk(view, tag, allocator)` returns a `ReadChunk`: bytes + entry + `read_error`;
`read_chunk(entry, out)` reads into caller memory;
`read_bytes(entry, offset, out, size)` reads a range of one chunk),
`matches(view)` catches a stale view, `read_prelude` (on the reader, or `ASSET_FILE::read_prelude(path, allocator)`) returns a parsed view over a fresh buffer released with `ASSET_FILE::free_prelude`.
`asset/source_chunk.hpp` is the `SRC ` chunk: the bytes an asset was imported from, preceded by the original file name (`SourceChunkHeader` + name + 16-byte aligned data;
`SOURCE_CHUNK::add_chunk` / `add_placeholder` write it, `has_source` tells a kept original from the size-0 placeholder, `SourceChunkView::parse(entry, bytes, size)` reads the name and the data's position from the first `SOURCE_CHUNK::PREFIX_SIZE` (256) bytes alone);
`MeshImportOptions` / `TextureImportOptions::keep_source` (default true) decides whether an importer keeps it, the payload writers ignore the flag.
`asset/asset_types/texture_asset.hpp` and `asset/asset_types/mesh_asset.hpp` define the texture and mesh payload layouts (plain on-disk structs, format enums, a validating `TextureAssetView` / `MeshAssetView` that takes the prelude view plus the descriptor chunk's bytes or `ReadChunk` and hands back the entries of the big chunks to read, and a `TextureAssetWriter` / `MeshAssetWriter` that builds the payloads and `add_chunks` them to an `AssetWriter`).
`asset/asset_resource_provider.hpp` is the `AssetResourceProvider`: assets are registered by prelude view plus path (`add(view, path)`, keyed by the header GUID, prelude copied),
`get(guid)` / `get(view)` lazily reads every runtime chunk into provider-owned 64-byte aligned buffers through `AssetReader` and returns the same `AssetResource` afterwards (`find_payload(tag, &entry)` gives the bytes to hand a per-type view),
`get_chunk` reads one chunk, `unload` streams payloads out;
a file re-imported since the view was taken is detected with `matches` and its prelude refreshed.
The runtime `Engine` creates it as a singleton in `init()` and frees it in `shutdown()`.
`asset/text_asset.hpp` is the other kind of asset file: hand-authored assets (`.material`, `.scene`) are UTF-8 text in a flat `key = value` / `[section]` syntax (`TextAssetCursor` walks the lines, `TEXT_ASSET::parse_guid` / `format_guid` / `parse_numbers` / `format_number` (shortest text that reads back to the same f32, what every text asset writes numbers with) / `next_word` the values) with a top-level `guid` (32 hex digits, hi then lo) and the type given by the extension (`TEXT_ASSET::type_of_path`, `MATERIAL_EXTENSION` / `SCENE_EXTENSION`);
`TEXT_ASSET::read_prelude(path)` synthesizes in memory the prelude the provider keys on (header flagged `ASSET_FLAG::TEXT | COOKED`, `content_hash` of the text, one `CHUNK_TYPE::TEXT` chunk of the file's bytes, no dependencies),
the provider reads such a file whole when its chunk is asked for (`is_text()` resources: `TEXT_ASSET::matches` is the staleness check) and `ASSET_FILE::read_prelude_any(path)` dispatches on the extension (what `Engine::load_asset_file` calls, so `.lunaasset`, `.material` and `.scene` register the same way).
`asset/asset_types/material_asset.hpp` is the `.material` layout: `MaterialAsset` (guid, shader name, `[params]` as name + up to 16 numbers, `[textures]` as name + texture GUID or `none`, `[samplers]` as name + `MaterialSamplerDesc` from words like `linear repeat anisotropy=8`;
core-only types, no GPU enums),
`MATERIAL_ASSET::parse(text, size, out, &line)` with a `MaterialParseError` per mistake (unknown keys and duplicates are errors) and `write` / `write_file` in a canonical layout with shortest-round-trip floats.
The file never states types: the graphics side (`MATERIAL::load`) checks each param against the shader's reflection.
`scene/serialize.hpp` is the component reflection layer a `.scene` file (and later prefabs and Luau) read and write through: a `Serializer` component (name, `write` / `read` function pointers, `init` for the default value, `order`) stored on the component entity itself like `Inspector`, set by `SERIALIZE::expose<T>(world, name, write, read, order)` (data components, `init` defaulting to a copy of `T{}`) or `SERIALIZE::expose_tag<T>(world, name)` (tags: no functions, listed in an entity's `tags` line), read back with `SERIALIZE::of(world, id)` and looked up by name with `SERIALIZE::find(world, name, length)` (a scan over the serializers for now);
a write function emits every field through `TextWriter` (`field(name)` prefixes the serializer's name, so `Transform.position = `) with the `SERIALIZE::write_f32` / `write_vec3` / `write_quat` / `write_bool` / `write_guid` / `write_word` / `write_entity_ref` helpers,
a read function is called per `Component.field` line with the field and value spans and returns false for an unknown field or a bad value (the matching `read_*` helpers take `(value, length, out)`);
`SerializeContext` carries the world, the entity, the file-local id callbacks the scene writer / reader fill, the caller's `resolve_asset` (GUID to entity) and `warn` sink with `path` / `line`, and counts `warnings`;
entity references go through `write_entity_ref` / `read_entity_ref` only: an entity of the scene being written is its file-local id, an entity with `AssetUuid` its GUID (32 hex digits; fewer digits are a local id), anything else `none` with a warning, and reading resolves a GUID through `resolve_asset` (falling back to `ASSET_ENTITY::find`, which loads nothing).
`asset/asset_types/scene_asset.hpp` is the `.scene` layout (`docs/asset_format.md` "Scene files"): `SCENE_ASSET::write(world, root, guid, ctx, out)` / `write_file` emit `guid` then every descendant of `root` in depth-first order as `[entity N]` sections (`name`, `parent = N`, components lowest `order` first, `tags`), N being file-local and regenerated at every save;
`SCENE_ASSET::read(world, text, size, parent, ctx)` instantiates the sections under `parent` in two passes (every section a fresh `new_entity()` with its Name and CHILD_OF, then the components and tags, so a parent or a reference may come after the section that names it;
a component first met on a line is `set` to its serializer's default before its fields are read) and returns a `SceneReadResult` (`ok` false, with `error` and `line`, only for a structural failure: no guid, a duplicate `[entity N]`, a malformed line, in which case nothing was created;
line problems warn through `ctx.warn` with `path:line` and skip the line).
`Scene::unload()` deletes the root's children (synchronous, the CHILD_OF cascade), `Scene::current` / `current_path` (`set_current`, `has_current`) record the scene asset the content came from, set by the Engine.
`utils/guid.hpp` is `GUID::random()`, a fresh non-null `AssetGuid` (what the editor's `IMPORT::random_guid` forwards to).
`asset/asset_entity.hpp` makes assets entities: the `AssetUuid` component (the GUID),
the exclusive `AssetType` relation whose target is one of the tags `AssetTexture` / `AssetMesh` / `AssetMaterial` / `AssetScene` / `AssetShader` (`ASSET_ENTITY::type_tag(world, ASSET_TYPE::*)` / `type_of(world, entity)` / `type_name` convert),
and a `Name` from the file stem (`ASSET_ENTITY::name_from_path`);
query them as `world.query<AssetUuid>().with<ECS::Pair<AssetType, AssetTexture>>()` (on `Query`, `with<A, B>()` is two separate terms, a pair is spelled `ECS::Pair`).
`AssetEntities` is the GUID-to-entity index that keeps one entity per asset (`init(world)` registers and names the components and installs the removed hook on `AssetUuid` that drops deleted entities;
`add(guid, type, name, path = nullptr)` / `add(view, path)` create or bring up to date, a non-null path also setting the `AssetPath` component (the file the asset was registered from, `ASSET_PATH_CAPACITY` 512, what a loader that reads the file itself opens), `find`, `remove` deletes the entity, `free`);
the `AssetDirty` tag says an asset entity has edits its file lacks (added and cleared by whoever loads that kind of file, the renderer for materials, never by core;
`world.query<Material>().with<AssetDirty>()` is the editor's Save All);
`ASSET_ENTITY::find(world, guid)` is the scan for code that only has the World.
Asset entities are unparented, so never scene content.
`docs/asset_format.md` is the specification and the place to record layout decisions.
`scheduler/scheduler.hpp` is the `Scheduler`, what runs when (`docs/plugins_and_scheduler_plan.md`): pipelines are the entry points an owner invokes at fixed moments (`add_pipeline(name, order)`, the Engine's startup / update / render / shutdown),
each a list of phases (`add_phase`), each a list of systems (`add_system(phase, name, SystemFn, user_data)`, a function pointer receiving a `SystemContext` with the run's `host` and `argument`, `dt`, its `user_data` and its handles);
`phase_after` / `phase_before` order phases of one pipeline, `system_after` / `system_before` systems of one phase, what is unconstrained keeps insertion order (or the reverse for a `PIPELINE_ORDER_REVERSE_INSERTION` pipeline, which teardown wants),
`run(pipeline, host, argument, dt)` calls every enabled system in order from a copy of it, so a system may remove, add or disable systems or run another pipeline;
handles carry generations (`SparseList` ids), names are unique per scope and looked up with `find_pipeline` / `find_phase` / `find_system` (how a plugin orders against another's step by name),
structural edits set `dirty` and the next run recompiles every pipeline, a cycle being reported once through `log` and marking only its pipeline `broken` (its `run` returns false until an edit removes the cycle);
`phase_order` / `system_order` report the resolved order;
no World, no Engine, single-threaded;
tested in `tests/core/scheduler/`.
`utils/hash.hpp` is `HASH::fnv1a` / `fnv1a_str` (constexpr) / `fnv1a_append`, the one hash for names and cache keys.
`geometry/primitives.hpp` is the built-in shapes: `PrimitiveShape` (`PRIMITIVE_CUBE` / `PRIMITIVE_SPHERE` / `PRIMITIVE_CYLINDER`, stored in components, never renumbered),
each unit-sized and centered (cube edges 1, sphere radius 0.5, cylinder radius 0.5 and height 1 along Y) so a Transform's scale is its size and physics can later build a collider from shape + scale alone;
`PRIMITIVES::build(shape, MeshAssetWriter&, PrimitiveTessellation)` generates the triangles in the OBJ importer's layout (one 32-byte interleaved position/normal/texcoord stream, counter-clockwise from outside, outward normals),
`vertex_count` / `index_count` / `name` / `is_valid` beside it;
tested in `tests/core/geometry/`.
`VERTEX_LAYOUT` in `mesh_asset.hpp` fixes the shader input location of every vertex semantic (position 0, normal 1, tangent 2, color 3, texcoord0..3 4..7, joints 8, weights 9) and hashes a mesh's vertex layout for pipeline caching.
`render_graph/render_graph.hpp` is the live `RenderGraph`: transient resources (`add_resource`, format defaulting to the backbuffer's, size relative or absolute) and passes (`add_pass` of kind DRAW_SCENE / FULLSCREEN / CLEAR / BLIT / CUSTOM) with color/depth attachments, sampled inputs, order (`move_pass`) and `enabled`;
the two built-in resources `backbuffer` / `backbuffer_depth` are the FrameContext target, written by a chain of passes in order (each stores the backbuffer, the first never loads it, later ones load it to draw over the frame;
`backbuffer_depth` is a target-sized depth any raster pass may take, never loaded by its first user and loadable later when the user before stored it), the last of them being `RenderGraphPlan::backbuffer_pass`.
Structural edits set `dirty`, live ones (clear values, constants, draw tag, fullscreen shader, custom callback) do not.
Handles carry generations, names are looked up by hash.
`render_graph/render_graph_plan.hpp` is the pure compiler: `RENDER_GRAPH::compile(graph, backbuffer, plan, diagnostics)` validates and produces a `RenderGraphPlan` (resolved resources with usage and frame start/end states, passes with per-attachment load/store/initial/final/previous states, the states inputs must be put in before each pass, and a render-pass compatibility key per raster pass that covers formats plus the dependency-defining states).
No Vulkan types in core: formats are opaque `u32`s.
One image per transient resource is shared by all frames in flight (one queue, one command buffer, render pass dependencies order the frames).
`scene/scene.hpp` is the scene hierarchy over the ECS: the `Name` component (`char value[ENTITY_NAME_CAPACITY]`, display only, never a lookup key),
the `SCENE::` helpers that read and write `(ECS::CHILD_OF, parent)` pairs and names on any `World` (`parent` / `set_parent` (exclusive, refuses cycles, 0 detaches) / `is_descendant_of` / `children` (direct children appended lowest id first) / `child_count` / `has_children` / `name` / `set_name` / `spawn(world, name, parent)`),
and `Scene`, whose `init(world)` creates the `scene_root` entity and whose `spawn(name, parent = 0)` parents under it by default;
`contains(entity)` walks up to the root.
Entities that are not scene content are simply left without a parent (`SCENE::spawn(world, name, 0)` or a bare `new_entity`),
so a walk down from `scene_root` only meets scene content.
Deleting an entity deletes its subtree (the `CHILD_OF` cleanup policy).
`scene/inspector.hpp` is how a component shows up in the editor's Inspector: an `Inspector` component (name, `InspectorDrawFn draw`, `order`) stored on the component entity itself by `INSPECTOR::expose<T>(world, name, draw, order)` and read back with `INSPECTOR::of(world, id)` (nullptr for pairs, tags and unexposed ids).
Core only declares `InspectorContext` and the function pointer type;
the editor defines both and registers its ImGui draw functions at startup.
A draw function gets the entity's data pointer, returns true if it wrote to it, and must never add or remove components.
Whether the Inspector's "Add Component" menu offers a component is the `Addable` component on the same component entity (`INSPECTOR::addable<T>(world, init)`: `init` is an `InspectorInitFn(world, entity, data)` that writes the default value into zeroed bytes, defaulting to a copy of a value-initialized `T`, none for tags);
`INSPECTOR::can_add(world, entity, id)` and `INSPECTOR::add(world, entity, id)` (builds the value in scratch, then `set`s it, so the added hook sees the final value;
tags are `add`ed) are the editor's way in, `addable_of` reads it back.
`Quaternion::to_euler` inverts `from_euler` (pitch, yaw, roll;
roll reported 0 at the gimbal lock).
