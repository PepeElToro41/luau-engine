// The outline effect, as the algorithm and nothing else: every pass of a
// jump-flood outline, each under the `PASS_<TAG>` define the shader path
// already hands every compile (shaders/preprocessing.hpp). A shader that
// wants a pass writes its `#pragma pass` line and this include; the body
// lives here once, so the editor's selection outline and a game's own
// outline share every line of it.
//
//     render/outline.slang           the three screen-space passes
//     render/editor/selection.slang  the editor's mask pass
//
// The four passes, and what the render graph has to give each one
// (render/outline.hpp builds exactly this chain):
//
//   outline_mask     Draws the outlined objects into the seed buffer: a
//                    fragment writes itself as its own nearest seed,
//                    together with the surface's window depth. Color 0 is
//                    the seed buffer cleared to 0 (which decodes as "no
//                    seed"), and its depth attachment is a scratch depth
//                    of its own, so the nearest front face wins where the
//                    silhouette overlaps itself. A mesh pass: it needs
//                    the object's push constants and a vertex layout.
//
//   outline_flood    One jump-flood iteration: input 0 is the seed buffer
//                    of the step before, color 0 the other one, and the
//                    push constant is this step's offset. Run it with the
//                    offsets S, S/2, ... 1, where S is a power of two at
//                    least the widest outline wanted; every pixel within
//                    S of the silhouette then holds (approximately) the
//                    nearest silhouette pixel and its depth.
//
//   outline_resolve  Turns that into the outline: the distance to the
//   outline_resolve_ nearest seed is the distance to the silhouette, so a
//   behind           band of it is drawn over the frame and everything
//                    else is discarded. Input 0 is the flooded buffer,
//                    color 0 the frame, and the depth attachment is the
//                    scene's own depth, loaded and not written: the
//                    fragment reports the *silhouette's* depth as
//                    SV_Depth, so the hardware depth test decides whether
//                    the outlined surface is in front of what is drawn
//                    there. `outline_resolve` takes the comparison that
//                    keeps the visible side (`depth_lequal`) and
//                    `outline_resolve_behind` the one that keeps the
//                    occluded side (`depth_greater`), which is the whole
//                    difference between them: same body, a fainter color
//                    in the push constants. A game that does not want
//                    outlines through walls simply never adds that
//                    second pass to its graph.
//
// The flood is pure bandwidth -- every iteration reads nine texels and
// writes one, over the whole target -- so the seed buffer is packed as
// tightly as the algorithm allows: RGBA16_UINT, eight bytes a texel.
// Pixel coordinates and plain Load() keep it exact and need no sampler.
#ifndef ENGINE_OUTLINE_SLANG
#define ENGINE_OUTLINE_SLANG

#include "engine/bindings.slang"

// --- The seed buffer ----------------------------------------------------
//
// A texel is (offset.x, offset.y, depth >> 16, depth & 0xffff): where the
// nearest silhouette pixel is *relative to this one*, and the window depth
// of the surface there as its exact float32 bit pattern.
//
// Relative, because an absolute coordinate needs as many bits as the
// target is wide while an offset never exceeds about twice the first flood
// step, so sixteen biased bits hold it exactly with room to spare (the
// step is at most 4096, so the offset is at most ~8192, half the bias).
// That is also what makes the validity test free: a cleared texel is all
// zeros, and no real offset encodes x as 0.
//
// The depth keeps all 32 of its bits deliberately. It is what the visible
// / occluded comparison tests against the scene's depth buffer, and a
// half-float near 1.0 steps by ~5e-4, which is many metres at the far end
// of a perspective projection -- the two resolve passes would disagree
// about the same pixel from frame to frame.
static const int OUTLINE_SEED_BIAS = 16384;

struct OutlineSeed {
    // Pixels from the fragment that read this texel to the seed.
    int2 offset;
    float depth;
    bool valid;
};

uint4 outline_encode(const int2 offset, const float depth) {
    const uint bits = asuint(depth);
    return uint4(uint(offset.x + OUTLINE_SEED_BIAS), uint(offset.y + OUTLINE_SEED_BIAS), bits >> 16, bits & 0xffffu);
}

OutlineSeed outline_decode(const uint4 texel) {
    OutlineSeed seed;
    seed.valid = texel.x != 0u;
    seed.offset = int2(int(texel.x) - OUTLINE_SEED_BIAS, int(texel.y) - OUTLINE_SEED_BIAS);
    seed.depth = asfloat((texel.z << 16) | texel.w);
    return seed;
}

int outline_squared_length(const int2 offset) {
    return offset.x * offset.x + offset.y * offset.y;
}

// --- Mask ---------------------------------------------------------------

#if defined(PASS_OUTLINE_MASK)

#include "engine/frame.slang"
#include "engine/object.slang"
#include "engine/vertex.slang"

struct OutlineMaskInput {
    VERTEX_POSITION float3 position;
};

struct OutlineMaskVertex {
    float4 clip_position : SV_Position;
};

[shader("vertex")]
OutlineMaskVertex vertex(OutlineMaskInput v) {
    OutlineMaskVertex o;
    o.clip_position = mul(frame.view_projection, mul(object.model, float4(v.position, 1.0)));
    return o;
}

// A covered pixel is its own nearest seed, at offset zero; SV_Position.z
// is the window depth of the surface there.
[shader("fragment")]
uint4 fragment(OutlineMaskVertex v) : SV_Target {
    return outline_encode(int2(0, 0), v.clip_position.z);
}

// --- Screen-space passes ------------------------------------------------

#elif defined(PASS_OUTLINE_FLOOD) || defined(PASS_OUTLINE_RESOLVE) || defined(PASS_OUTLINE_RESOLVE_BEHIND)

#include "engine/fullscreen.slang"

PASS_INPUT(0) Texture2D<uint4> outline_seeds;

// The pixel a fragment covers, and the size of the seed buffer.
int2 outline_pixel(const FullscreenVertex v) {
    return int2(v.clip_position.xy);
}

int2 outline_extent() {
    uint width;
    uint height;
    outline_seeds.GetDimensions(width, height);
    return int2(int(width), int(height));
}

#if defined(PASS_OUTLINE_FLOOD)

struct OutlineFloodConstants {
    // This iteration's offset in pixels.
    int step;
};
[[vk::push_constant]] ConstantBuffer<OutlineFloodConstants> outline_flood;

// One jump-flood iteration: of this pixel's own seed and the seeds of the
// eight neighbours `step` pixels away, keep the one nearest to this pixel.
// A neighbour's offset is relative to the neighbour, so it is re-based on
// this pixel before it is compared and stored.
[shader("fragment")]
uint4 fragment(FullscreenVertex v) : SV_Target {
    const int2 pixel = outline_pixel(v);
    const int2 extent = outline_extent();

    uint4 best = uint4(0, 0, 0, 0);
    int best_distance = 0x7fffffff;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const int2 at = pixel + int2(x, y) * outline_flood.step;
            if (at.x < 0 || at.y < 0 || at.x >= extent.x || at.y >= extent.y) {
                continue;
            }
            const OutlineSeed seed = outline_decode(outline_seeds.Load(int3(at, 0)));
            if (!seed.valid) {
                continue;
            }
            const int2 offset = at + seed.offset - pixel;
            const int squared = outline_squared_length(offset);
            if (squared < best_distance) {
                best_distance = squared;
                best = outline_encode(offset, seed.depth);
            }
        }
    }
    return best;
}

#else // the two resolve passes

struct OutlineResolveConstants {
    // rgb the outline's color, a its peak opacity.
    float4 color;
    // Half-width of the band in pixels, and how much of its outer edge is
    // feathered (also in pixels) so the outline is not aliased.
    float width;
    float softness;
};
[[vk::push_constant]] ConstantBuffer<OutlineResolveConstants> outline;

// The flooded buffer holds the nearest silhouette pixel; its distance is
// the distance to the silhouette. A band of it is the outline, the
// silhouette itself (offset zero: the pixel is its own seed) and
// everything past the band are discarded, and the silhouette's depth is
// reported as this fragment's so the pipeline's depth comparison decides
// whether this is the visible or the occluded side of the object.
[shader("fragment")]
void fragment(FullscreenVertex v, out float4 color : SV_Target, out float depth : SV_Depth) {
    const int2 pixel = outline_pixel(v);
    const OutlineSeed seed = outline_decode(outline_seeds.Load(int3(pixel, 0)));
    if (!seed.valid) {
        discard;
    }
    const float distance_to_seed = sqrt(float(outline_squared_length(seed.offset)));
    if (distance_to_seed < 0.5 || distance_to_seed > outline.width) {
        discard;
    }
    const float coverage = saturate((outline.width - distance_to_seed) / max(outline.softness, 1.0e-4));
    color = float4(outline.color.rgb, outline.color.a * coverage);
    depth = seed.depth;
}

#endif // PASS_OUTLINE_FLOOD

#endif // the pass branches

#endif // ENGINE_OUTLINE_SLANG
