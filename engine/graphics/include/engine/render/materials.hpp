#pragma once

#include "engine/asset/asset_types/material_asset.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/gpu/gpu_types.hpp"
#include "engine/math/math.hpp"

// Materials as entities: a Material component with CPU values for its
// shader's set-2 interface. Values are set by name through the shader's
// reflection (a lookup and a memcpy into the block); textures and samplers
// by the name of their binding. Nothing reaches the GPU until the material
// is drawn, when the frame pushes the block through the uniform ring and
// builds a transient bind group.
//
// A material can also come from a `.material` file (a text asset, see
// engine/asset/asset_types/material_asset.hpp): load(renderer, guid) takes
// the asset through the AssetResourceProvider, loads its shader by name,
// creates the entity and applies every value, texture and sampler it
// names, reporting the ones the shader does not declare. The same asset
// loads once; reload() re-reads the file into the same entity, and save()
// writes an entity's current values back as a file.

struct Renderer;
struct Material;
struct Shader;

namespace MATERIAL {

// A new entity with a Material for `shader`, every value zero, every
// texture white, every sampler linear. 0 if `shader` is not a Shader entity.
EntityId create(Renderer& renderer, EntityId shader);
// Set a member of the material block by name. False (with a message) if
// the member does not exist or has another type.
bool set_float(Renderer& renderer, EntityId material, const char* name, f32 value);
bool set_int(Renderer& renderer, EntityId material, const char* name, i32 value);
bool set_vec2(Renderer& renderer, EntityId material, const char* name, Vector2 value);
bool set_vec3(Renderer& renderer, EntityId material, const char* name, Vector3 value);
bool set_vec4(Renderer& renderer, EntityId material, const char* name, Vector4 value);
bool set_mat4(Renderer& renderer, EntityId material, const char* name, const Matrix4x4& value);
// Binds a texture asset to a Texture2D of set 2 by name.
bool set_texture(Renderer& renderer, EntityId material, const char* name, const AssetGuid& texture);
// Sets a SamplerState of set 2 by name.
bool set_sampler(Renderer& renderer, EntityId material, const char* name, const GpuSamplerDesc& sampler);

// Rebuilds the material's block and slots for the shader's current
// generation if they are stale. Called before every draw.
void sync(Material& material, const Shader& shader);

// --- Files ---------------------------------------------------------------------

// The Material entity for the .material asset `asset`, registered with the
// renderer's AssetResourceProvider (Engine::load_asset_file): the one
// loaded before, or a new entity named after the file with the asset's
// values applied. 0 (with messages) when the asset is unknown, does not
// parse or its shader does not load; a value the shader does not declare
// or of another shape is reported and skipped, the rest still applies.
// The text payload is released from the provider after the load.
EntityId load(Renderer& renderer, const AssetGuid& asset);
// Re-reads the file of a loaded material into the same entity: every value
// reset, the file applied again, the shader switched if the file names
// another. False (the entity untouched) when it was not loaded from a file
// or the file no longer parses.
bool reload(Renderer& renderer, EntityId material);
// Reloads every material that came from a file.
void reload_all(Renderer& renderer);

// The entity's current state as a MaterialAsset: its shader's name, every
// block member, texture slot and sampler slot of the material interface,
// under `Material::asset` as the guid (null for a material built in code;
// set `out.guid` before writing it). False if `material` is not a material
// entity.
bool to_asset(Renderer& renderer, EntityId material, MaterialAsset& out);
// Writes the entity's current state to `path` as a .material file. The
// material must have a guid: its asset's, or one assigned to
// Material::asset beforehand. False (with a message) otherwise or when the
// file cannot be written.
bool save(Renderer& renderer, EntityId material, const char* path);

// The GPU sampler a file's sampler words describe, and back.
GpuSamplerDesc sampler_desc(const MaterialSamplerDesc& sampler);
MaterialSamplerDesc sampler_asset(const GpuSamplerDesc& sampler);

} // namespace MATERIAL
