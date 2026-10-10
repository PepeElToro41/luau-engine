#pragma once

#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/defines.hpp"

// The built-in primitive shapes: a cube, a sphere and a cylinder, each of
// unit size centered on the origin so a Transform's scale is its size.
// A PrimitiveRenderer names one of them instead of a mesh asset, and since
// the shape is a closed form rather than geometry, physics can later build
// colliders from the shape plus the entity's scale without touching any
// triangle. The triangles are only needed to draw: PRIMITIVES::build makes
// them in the same layout the OBJ importer writes (one interleaved stream
// of position, normal and texcoord, 32 bytes per vertex), so the renderer
// uploads them through the same path as an imported mesh.
//
//     MeshAssetWriter mesh;
//     if (PRIMITIVES::build(PRIMITIVE_SPHERE, mesh) == MESH_WRITE_OK) { ... upload ... }
//     mesh.free();
//
// Dimensions, all in object space:
//
//     PRIMITIVE_CUBE      edges of length 1, from -0.5 to 0.5 on every axis
//     PRIMITIVE_SPHERE    radius 0.5
//     PRIMITIVE_CYLINDER  radius 0.5, height 1 along Y (caps at y = -0.5 and 0.5)
//
// Every face is wound counter-clockwise seen from outside, normals point
// outward (per face on the cube, radial on the sphere and the cylinder's
// side, axial on its caps). Texcoords: each cube face spans 0..1 with v = 0
// at the top edge; the sphere and the cylinder's side wrap u once around Y
// (u = 0 at +Z, growing toward +X) with v = 0 at the top; the cylinder's
// caps map their disc into 0..1.

// Stored in components: never renumber, only append.
enum PrimitiveShape : u32 {
    PRIMITIVE_CUBE = 0,
    PRIMITIVE_SPHERE = 1,
    PRIMITIVE_CYLINDER = 2,
    PRIMITIVE_COUNT = 3,
};

// How finely the round shapes are tessellated. The cube ignores it.
struct PrimitiveTessellation {
    // Divisions around the Y axis (sphere longitude, cylinder side). At
    // least 3.
    u32 segments = 32;
    // Divisions from pole to pole on the sphere. At least 2.
    u32 rings = 16;
};

namespace PRIMITIVES {

// "cube", "sphere", "cylinder"; "unknown" for anything else.
const char* name(PrimitiveShape shape);
bool is_valid(PrimitiveShape shape);

// Builds `shape` into `out` (see the header comment for the layout). The
// tessellation is clamped to its minimums. MESH_WRITE_BAD_SOURCE for an
// unknown shape.
MeshWriteError build(PrimitiveShape shape, MeshAssetWriter& out, const PrimitiveTessellation& tessellation = PrimitiveTessellation{});

// The vertex and index counts build() produces for the shape, so a caller
// can size things or a test can check them.
u32 vertex_count(PrimitiveShape shape, const PrimitiveTessellation& tessellation = PrimitiveTessellation{});
u32 index_count(PrimitiveShape shape, const PrimitiveTessellation& tessellation = PrimitiveTessellation{});

} // namespace PRIMITIVES
