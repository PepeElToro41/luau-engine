#pragma once

#include "ui/inspectors/inspector_context.hpp"

struct World;

// The draw functions the editor registers on the engine's components (see
// engine/scene/inspector.hpp), one source file each under ui/inspectors/.
// Each takes the component's data and returns true if it changed it.
namespace INSPECTORS {

// Exposes every component below on `world` and marks Transform, Camera and
// PrimitiveRenderer addable (see Addable in engine/scene/inspector.hpp).
// Call once after Engine::init.
void register_all(World& world);

// The value "Add Component" gives a PrimitiveRenderer: a cube with the
// lowest-id Material entity in the world (0 when there is none).
void default_primitive_renderer(World& world, EntityId entity, void* data);

// Transform: position, rotation as Euler degrees (kept across frames in the
// scratch so the fields do not jump while editing), scale.
bool transform(InspectorContext& ctx, void* data);
// Camera: vertical field of view in degrees, near and far planes.
bool camera(InspectorContext& ctx, void* data);
// PrimitiveRenderer: the shape as a combo, the material entity by name
// (read-only until materials can be picked).
bool primitive_renderer(InspectorContext& ctx, void* data);
// Material: the shader by name, then every member of its material block
// as a drag / color / checkbox field by the reflected type, the texture
// slots as a combo over the texture asset entities (every AssetUuid with
// (AssetType, AssetTexture), by Name, plus `none`; the GUID in the
// tooltip) and the sampler slots as their words, written straight into
// the Material's block and slots.
bool material(InspectorContext& ctx, void* data);
// AssetUuid: the asset's type (from its AssetType pair) and GUID, read-only.
bool asset_uuid(InspectorContext& ctx, void* data);

} // namespace INSPECTORS
