#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"

// Shaders as entities. A shader is found by name: the AssetPath of its
// asset entity when one is registered (see below), else <project render
// dir>/<name>.slang (then .glsl), then the same under the engine render
// dir. The two render directories are the include roots either way.
// Loading the same name twice returns the same entity. The entity is named
// but unparented, so it never shows up in the scene tree.
//
// A shader file can also be an asset entity before it is loaded (AssetUuid
// with (AssetType, AssetShader), see asset/asset_entity.hpp): its GUID is
// asset_guid(name), derived from the name since the file carries none, and
// load() puts the Shader on that entity when it exists, so a shader file,
// its asset and its Shader are one entity, the way MATERIAL::load lands on
// a .material's asset entity. The editor registers every shader file of
// the engine's render directory and of the whole project this way, with
// its path, so a project shader may live in any folder, and a project file
// named like an engine one takes its entity and path over (the override).

struct Renderer;

namespace SHADER_LIBRARY {

// The GUID shader `name` has as an asset: a hash of the name, the same on
// every run. Null for an empty name.
AssetGuid asset_guid(const char* name);
// Loads <name>.slang (or .glsl) into an entity with a Shader component:
// the asset entity for asset_guid(name) when the World has one, a new named
// entity otherwise. 0 on failure, with the compiler's log sent to the
// renderer's log.
EntityId load(Renderer& renderer, const char* name);
// Recompiles the entity's file. On success the new program replaces the
// old one in place, its pipelines are released and `generation` bumps; on
// failure the old program keeps drawing and the log is reported.
bool reload(Renderer& renderer, EntityId shader);
// Reloads every Shader entity.
void reload_all(Renderer& renderer);

} // namespace SHADER_LIBRARY
