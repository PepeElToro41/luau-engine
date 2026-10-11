// The editor Viewport's ground grid: the y = 0 plane as two triangles built
// from the vertex index (no vertex buffer), drawn in the "alpha" pass after
// the scene with alpha blending, depth-tested against what the scene left
// in the depth buffer and never writing it, so objects occlude the grid and
// the grid never occludes anything. Minor lines every unit, major lines
// every ten, the X axis red and the Z axis blue, all fading with distance
// so the far edge and the aliasing of distant minor lines are never seen.
// Editor-only: the standalone never loads it.
#pragma pass grid_editor vertex fragment blend depth_test no_depth_write no_cull
#include "engine/frame.slang"

// Half-size of the quad in world units; what the camera's far plane does
// not clip the fade hides anyway.
static const float EXTENT = 10000.0;
static const float FADE_DISTANCE = 1000.0;
static const float MINOR_SPACING = 1.0;
static const float MAJOR_SPACING = 10.0;
static const float3 MINOR_COLOR = float3(0.30, 0.30, 0.30);
static const float3 MAJOR_COLOR = float3(0.48, 0.48, 0.48);
static const float3 X_AXIS_COLOR = float3(0.85, 0.25, 0.25);
static const float3 Z_AXIS_COLOR = float3(0.25, 0.45, 0.90);

// Two counter-clockwise triangles seen from +Y.
static const float2 CORNERS[6] = {
    float2(-1.0, -1.0), float2(1.0, 1.0), float2(1.0, -1.0),
    float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0),
};

struct GridVertex {
    float4 clip_position : SV_Position;
    float3 world_position;
};

[shader("vertex")]
GridVertex vertex(uint id : SV_VertexID) {
    const float2 corner = CORNERS[id] * EXTENT;
    GridVertex o;
    o.world_position = float3(corner.x, 0.0, corner.y);
    o.clip_position = mul(frame.view_projection, float4(o.world_position, 1.0));
    return o;
}

// Coverage (0..1) of a one-pixel-wide line every `spacing` units of `p`,
// anti-aliased from the screen-space derivative. Lines closer together than
// a few pixels fade out instead of merging into a solid fill.
float grid_coverage(float2 p, float spacing) {
    const float2 coord = p / spacing;
    const float2 d = fwidth(coord);
    const float2 g = abs(frac(coord - 0.5) - 0.5) / d;
    const float line_distance = min(g.x, g.y);
    const float coverage = 1.0 - min(line_distance, 1.0);
    const float density_fade = saturate(1.0 - 4.0 * max(d.x, d.y));
    return coverage * density_fade;
}

// Coverage of the line `coord == 0` (one world axis), one pixel wide.
float axis_coverage(float coord) {
    return 1.0 - min(abs(coord) / fwidth(coord), 1.0);
}

[shader("fragment")]
float4 fragment(GridVertex v) : SV_Target {
    const float2 p = v.world_position.xz;
    const float minor = grid_coverage(p, MINOR_SPACING);
    const float major = grid_coverage(p, MAJOR_SPACING) * 0.6;
    const float x_axis = axis_coverage(p.y); // z == 0
    const float z_axis = axis_coverage(p.x); // x == 0

    float3 color = MINOR_COLOR;
    float alpha = minor * 0.1;
    color = lerp(color, MAJOR_COLOR, major);
    alpha = max(alpha, major);
    color = lerp(color, X_AXIS_COLOR, x_axis);
    alpha = max(alpha, x_axis);
    color = lerp(color, Z_AXIS_COLOR, z_axis);
    alpha = max(alpha, z_axis);

    const float dist = distance(frame.camera_position.xyz, v.world_position);
    const float fade = 1.0 - saturate(dist / FADE_DISTANCE);
    return float4(color, alpha * fade * fade);
}
