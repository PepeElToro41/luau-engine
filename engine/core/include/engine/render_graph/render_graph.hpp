#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/templates/hash_map.hpp"
#include "engine/templates/sparse_list.hpp"

// The frame as data: which passes run, in what order, and which images they
// write, read and clear. The graph is a live object mutated through this API
// (by C++ today, by Luau later); RENDER_GRAPH::compile (render_graph_plan.hpp)
// turns it into a RenderGraphPlan that the graphics backend realizes with
// Vulkan render passes, framebuffers and barriers. Nothing here knows Vulkan:
// formats are opaque VkFormat values, image states are the engine's own enum.
//
//     RenderGraph graph;
//     graph.init(allocator);                    // creates "backbuffer" and "backbuffer_depth"
//
//     RenderResourceDesc color_desc;
//     color_desc.aspect = RENDER_ASPECT_COLOR;  // format defaults to the backbuffer's, size to 1x the backbuffer
//     RenderResourceHandle scene_color = graph.add_resource("scene_color", color_desc);
//
//     RenderPassHandle forward = graph.add_pass("forward", RENDER_PASS_DRAW_SCENE);
//     graph.set_draw_scene_tag(forward, "forward");
//     graph.set_color_attachment(forward, 0, scene_color, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
//     graph.set_depth_attachment(forward, graph.backbuffer_depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
//
//     RenderPassHandle post = graph.add_pass("post", RENDER_PASS_FULLSCREEN);
//     graph.add_input(post, scene_color);
//     graph.set_color_attachment(post, 0, graph.backbuffer, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
//
// Two kinds of edit. Structural ones (passes, resources, attachments,
// inputs, order, enabled) set `dirty`: the next frame recompiles the plan and
// rebuilds whatever Vulkan objects changed. Live ones (clear values, push
// constants, the scene tag, the fullscreen shader, the custom callback) are
// read every frame straight from the descs and never recompile anything.
//
// Handles carry a generation: one taken on a pass or resource that was since
// removed stops matching, and every call given such a handle returns false
// or nullptr rather than touching something else. Names are looked up by
// hash with a string compare on the hit, so a name is only ever its own.
//
// The two built-in resources are the FrameContext's target: `backbuffer` is
// its color image and `backbuffer_depth` its depth image. Exactly one pass
// writes the backbuffer, with one color attachment loaded CLEAR and stored,
// and depth (if it has it) loaded CLEAR and not stored, because that pass
// runs through the target's own render pass. The backbuffer is never read
// and `backbuffer_depth` belongs to that pass alone.

namespace RENDER_GRAPH {

constexpr u32 NAME_MAX = 32;
constexpr u32 MAX_COLOR_ATTACHMENTS = 8;
constexpr u32 MAX_INPUTS = 8;
constexpr u32 MAX_CONSTANT_BYTES = 128;
constexpr u64 INVALID_HANDLE = 0xffffffffffffffffull;

} // namespace RENDER_GRAPH

// --- Handles -----------------------------------------------------------------

struct RenderPassHandle {
    u64 id = RENDER_GRAPH::INVALID_HANDLE;

    bool is_valid() const { return this->id != RENDER_GRAPH::INVALID_HANDLE; }
    bool operator==(const RenderPassHandle& other) const { return this->id == other.id; }
    bool operator!=(const RenderPassHandle& other) const { return this->id != other.id; }
};

struct RenderResourceHandle {
    u64 id = RENDER_GRAPH::INVALID_HANDLE;

    bool is_valid() const { return this->id != RENDER_GRAPH::INVALID_HANDLE; }
    bool operator==(const RenderResourceHandle& other) const { return this->id == other.id; }
    bool operator!=(const RenderResourceHandle& other) const { return this->id != other.id; }
};

// --- Resources ---------------------------------------------------------------

enum RenderResourceKind : u32 {
    // An image the graph owns, created to fit its uses.
    RENDER_RESOURCE_TRANSIENT = 0,
    // The FrameContext target's color image.
    RENDER_RESOURCE_BACKBUFFER = 1,
    // The FrameContext target's depth image.
    RENDER_RESOURCE_BACKBUFFER_DEPTH = 2,
};

enum RenderResourceAspect : u32 {
    RENDER_ASPECT_COLOR = 0,
    RENDER_ASPECT_DEPTH = 1,
};

enum RenderSizeMode : u32 {
    // backbuffer extent * scale, at least 1x1.
    RENDER_SIZE_RELATIVE = 0,
    // width x height as given.
    RENDER_SIZE_ABSOLUTE = 1,
};

// A VkFormat value, kept opaque so core does not include Vulkan.
using RenderFormat = u32;
constexpr RenderFormat RENDER_FORMAT_UNDEFINED = 0;
// Resolved at compile time to the backbuffer's color format, or to its depth
// format for a depth resource.
constexpr RenderFormat RENDER_FORMAT_BACKBUFFER = 0xffffffffu;

struct RenderResourceDesc {
    char name[RENDER_GRAPH::NAME_MAX] = {};
    RenderResourceKind kind = RENDER_RESOURCE_TRANSIENT;
    RenderResourceAspect aspect = RENDER_ASPECT_COLOR;
    RenderFormat format = RENDER_FORMAT_BACKBUFFER;
    RenderSizeMode size_mode = RENDER_SIZE_RELATIVE;
    f32 scale = 1.0f;
    u32 width = 0;
    u32 height = 0;
};

// --- Passes ------------------------------------------------------------------

enum RenderLoadOp : u32 {
    RENDER_LOAD_CLEAR = 0,
    RENDER_LOAD_LOAD = 1,
    RENDER_LOAD_DONT_CARE = 2,
};

enum RenderStoreOp : u32 {
    RENDER_STORE_STORE = 0,
    RENDER_STORE_DONT_CARE = 1,
};

enum RenderPassKind : u32 {
    // Draws every renderable whose shader has a variant for the pass's tag.
    RENDER_PASS_DRAW_SCENE = 0,
    // One triangle covering the target with a fragment shader; the inputs
    // are bound as textures. Post-processing and compositing.
    RENDER_PASS_FULLSCREEN = 1,
    // Clears an image outside any render pass.
    RENDER_PASS_CLEAR = 2,
    // Copies (and scales) one image into another.
    RENDER_PASS_BLIT = 3,
    // A callback registered by name on the renderer. Raster if it has
    // attachments (the callback records inside the render pass), otherwise
    // free-form.
    RENDER_PASS_CUSTOM = 4,
    // Reserved; compile() rejects it until compute pipelines exist.
    RENDER_PASS_COMPUTE = 5,
};

struct RenderAttachment {
    RenderResourceHandle resource;
    RenderLoadOp load = RENDER_LOAD_CLEAR;
    RenderStoreOp store = RENDER_STORE_STORE;
    // Color: rgba. Depth: clear[0] is the depth, clear_stencil the stencil.
    f32 clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    u32 clear_stencil = 0;
};

struct RenderPassDesc {
    char name[RENDER_GRAPH::NAME_MAX] = {};
    RenderPassKind kind = RENDER_PASS_DRAW_SCENE;
    bool enabled = true;

    RenderAttachment color[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS] = {};
    u32 color_count = 0;
    RenderAttachment depth;
    bool has_depth = false;
    // Sampled by the pass (bound as descriptor set 1, binding i).
    RenderResourceHandle inputs[RENDER_GRAPH::MAX_INPUTS] = {};
    u32 input_count = 0;

    // Live data handed to the pass every frame: push constants for
    // FULLSCREEN, whatever the callback wants for CUSTOM.
    u8 constants[RENDER_GRAPH::MAX_CONSTANT_BYTES] = {};
    u32 constant_size = 0;

    // DRAW_SCENE: which shader variant draws, e.g. "forward" or "shadow".
    char tag[RENDER_GRAPH::NAME_MAX] = {};
    u64 tag_hash = 0;
    // FULLSCREEN: the shader entity (0 = none).
    EntityId shader = 0;
    // CUSTOM: the callback registered under this name.
    char callback[RENDER_GRAPH::NAME_MAX] = {};
    u64 callback_hash = 0;
    // CLEAR: the image and value.
    RenderResourceHandle clear_target;
    f32 clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    f32 clear_depth = 1.0f;
    u32 clear_stencil = 0;
    // BLIT: source and destination.
    RenderResourceHandle blit_src;
    RenderResourceHandle blit_dst;
    bool blit_linear = true;
};

// --- Graph -------------------------------------------------------------------

struct RenderGraph {
    // Empties the graph and creates the two backbuffer resources.
    void init(BaseAllocator* allocator = nullptr);
    void free();

    // --- Resources ---------------------------------------------------------

    // A transient resource named `name` described by `desc` (whose own
    // `name` and `kind` are overwritten). Invalid if the name is empty, too
    // long or taken.
    RenderResourceHandle add_resource(const char* name, const RenderResourceDesc& desc);
    // False for a stale handle or a backbuffer resource. Passes that still
    // reference the resource keep their stale handle and fail to compile.
    bool remove_resource(RenderResourceHandle handle);
    RenderResourceHandle find_resource(const char* name) const;
    // The desc, or nullptr for a stale handle. Editing a desc through the
    // pointer is a structural change: call mark_dirty() afterwards.
    RenderResourceDesc* resource(RenderResourceHandle handle);
    const RenderResourceDesc* resource(RenderResourceHandle handle) const;

    // --- Passes ------------------------------------------------------------

    // A pass named `name` of `kind`, appended to the order. Invalid if the
    // name is empty, too long or taken.
    RenderPassHandle add_pass(const char* name, RenderPassKind kind);
    bool remove_pass(RenderPassHandle handle);
    RenderPassHandle find_pass(const char* name) const;
    RenderPassDesc* pass(RenderPassHandle handle);
    const RenderPassDesc* pass(RenderPassHandle handle) const;
    // Moves `handle` to run right before `before`, or last when `before` is
    // invalid.
    bool move_pass(RenderPassHandle handle, RenderPassHandle before);

    // Structural edits: each sets `dirty`.
    bool set_color_attachment(RenderPassHandle pass, u32 index, RenderResourceHandle resource, RenderLoadOp load, RenderStoreOp store);
    // Drops attachment `index` and shifts the ones after it down.
    bool remove_color_attachment(RenderPassHandle pass, u32 index);
    bool set_depth_attachment(RenderPassHandle pass, RenderResourceHandle resource, RenderLoadOp load, RenderStoreOp store);
    bool clear_depth_attachment(RenderPassHandle pass);
    // Appends `resource` to the pass's inputs; false if already there or full.
    bool add_input(RenderPassHandle pass, RenderResourceHandle resource);
    bool remove_input(RenderPassHandle pass, RenderResourceHandle resource);
    bool set_enabled(RenderPassHandle pass, bool enabled);
    bool set_clear_target(RenderPassHandle pass, RenderResourceHandle resource);
    bool set_blit(RenderPassHandle pass, RenderResourceHandle src, RenderResourceHandle dst, bool linear);

    // Live edits: no recompile.
    bool set_clear_color(RenderPassHandle pass, u32 index, const f32 rgba[4]);
    bool set_clear_depth(RenderPassHandle pass, f32 depth, u32 stencil);
    // At most RENDER_GRAPH::MAX_CONSTANT_BYTES bytes.
    bool set_constants(RenderPassHandle pass, const void* data, u32 size);
    bool set_draw_scene_tag(RenderPassHandle pass, const char* tag);
    bool set_fullscreen_shader(RenderPassHandle pass, EntityId shader);
    bool set_custom_callback(RenderPassHandle pass, const char* name);

    // Marks the graph for recompilation; the structural setters call it.
    void mark_dirty();

    // --- Members -----------------------------------------------------------

    BaseAllocator* allocator = nullptr;
    SparseList<RenderPassDesc> passes;
    SparseList<RenderResourceDesc> resources;
    // Execution order, every alive pass exactly once.
    DynamicArray<RenderPassHandle> order;
    HashMap<u64, RenderPassHandle> pass_names;
    HashMap<u64, RenderResourceHandle> resource_names;
    RenderResourceHandle backbuffer;
    RenderResourceHandle backbuffer_depth;
    bool dirty = true;
    // Bumped by every structural edit; the compiler records which version a
    // plan was built from.
    u32 version = 0;

private:
    RenderResourceHandle add_resource_of_kind(const char* name, const RenderResourceDesc& desc, RenderResourceKind kind);
};
