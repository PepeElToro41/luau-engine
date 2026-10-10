#pragma once

#include "engine/defines.hpp"
#include "engine/templates/sparse_list.hpp"

// The backend's vocabulary: everything the frontend needs to describe,
// create and refer to GPU objects, with no API type in sight. Vulkan,
// Direct3D 12 and Metal each map these onto their own enums and objects
// under src/gpu/<backend>/. No functions live here; see gpu.hpp.
//
// Resources are small structs the frontend copies freely: a SparseId into
// a pool the backend owns, plus the API-neutral facts about the object
// (size, format, extent, usage) so that nothing has to call into the
// backend to read them. The id alone is what the backend resolves; the
// rest is informational and set at creation. An id of GPU_NULL_ID is "no
// object": backends reserve the zero slot of every pool so a real id is
// never 0.

static constexpr u32 FRAMES_IN_FLIGHT = 2;
static constexpr SparseId GPU_NULL_ID = 0;

enum GpuBackendKind : u32 {
    GPU_BACKEND_VULKAN = 0,
    GPU_BACKEND_D3D12 = 1,
    GPU_BACKEND_METAL = 2,
};

// --- Formats -----------------------------------------------------------------

// One enum for vertex attributes, textures and depth. The asset formats
// (VertexFormat, TextureFormat in asset_types/) map onto it with
// GPU_FORMAT::from_vertex / from_texture, and the render graph's
// RenderFormat is one of these values.
enum GpuFormat : u32 {
    GPU_FORMAT_UNDEFINED = 0,

    // 8-bit
    GPU_FORMAT_R8_UNORM,
    GPU_FORMAT_RG8_UNORM,
    GPU_FORMAT_RGBA8_UNORM,
    GPU_FORMAT_RGBA8_SRGB,
    GPU_FORMAT_RGBA8_SNORM,
    GPU_FORMAT_RGBA8_UINT,
    GPU_FORMAT_BGRA8_UNORM,
    GPU_FORMAT_BGRA8_SRGB,

    // 16-bit
    GPU_FORMAT_R16_UNORM,
    GPU_FORMAT_RG16_UNORM,
    GPU_FORMAT_RGBA16_UNORM,
    GPU_FORMAT_RG16_SNORM,
    GPU_FORMAT_RGBA16_SNORM,
    GPU_FORMAT_RGBA16_UINT,
    GPU_FORMAT_R16_FLOAT,
    GPU_FORMAT_RG16_FLOAT,
    GPU_FORMAT_RGBA16_FLOAT,

    // 32-bit
    GPU_FORMAT_R32_UINT,
    GPU_FORMAT_R32_FLOAT,
    GPU_FORMAT_RG32_FLOAT,
    GPU_FORMAT_RGB32_FLOAT,
    GPU_FORMAT_RGBA32_FLOAT,

    // Block compressed
    GPU_FORMAT_BC1_RGB_UNORM,
    GPU_FORMAT_BC1_RGB_SRGB,
    GPU_FORMAT_BC3_UNORM,
    GPU_FORMAT_BC3_SRGB,
    GPU_FORMAT_BC4_UNORM,
    GPU_FORMAT_BC5_UNORM,
    GPU_FORMAT_BC6H_UFLOAT,
    GPU_FORMAT_BC7_UNORM,
    GPU_FORMAT_BC7_SRGB,

    // Depth / stencil
    GPU_FORMAT_D16_UNORM,
    GPU_FORMAT_D32_FLOAT,
    GPU_FORMAT_D24_UNORM_S8_UINT,
    GPU_FORMAT_D32_FLOAT_S8_UINT,

    GPU_FORMAT_COUNT,
};

namespace GPU_FORMAT {

// Bytes per texel, or per block for compressed formats (see is_compressed).
u32 size(GpuFormat format);
bool is_depth(GpuFormat format);
bool has_stencil(GpuFormat format);
bool is_srgb(GpuFormat format);
bool is_compressed(GpuFormat format);
// Vertex formats whose components are integers (joint indices): the shader
// input must be declared int/uint, not float.
bool is_integer(GpuFormat format);
GpuFormat from_vertex(u32 vertex_format);   // VertexFormat, mesh_asset.hpp
GpuFormat from_texture(u32 texture_format); // TextureFormat, texture_asset.hpp
const char* name(GpuFormat format);

} // namespace GPU_FORMAT

// --- States and usage ----------------------------------------------------------

// What a texture is being used as. Barriers move a texture from one state
// to the next (cmd_barrier); the render graph planner already computes the
// state every pass needs each resource in. Vulkan derives image layouts and
// stage/access masks, D3D12 resource states, Metal mostly nothing.
enum GpuResourceState : u32 {
    GPU_STATE_UNDEFINED = 0, // contents may be discarded
    GPU_STATE_COLOR_ATTACHMENT = 1,
    GPU_STATE_DEPTH_ATTACHMENT = 2,
    GPU_STATE_SAMPLED = 3,
    GPU_STATE_TRANSFER_SRC = 4,
    GPU_STATE_TRANSFER_DST = 5,
    GPU_STATE_STORAGE = 6,
    GPU_STATE_PRESENT = 7,
};

enum GpuBufferUsage : u32 {
    GPU_BUFFER_USAGE_VERTEX = 1u << 0,
    GPU_BUFFER_USAGE_INDEX = 1u << 1,
    GPU_BUFFER_USAGE_UNIFORM = 1u << 2,
    GPU_BUFFER_USAGE_STORAGE = 1u << 3,
    GPU_BUFFER_USAGE_TRANSFER_SRC = 1u << 4,
    GPU_BUFFER_USAGE_TRANSFER_DST = 1u << 5,
};

enum GpuTextureUsage : u32 {
    GPU_TEXTURE_USAGE_SAMPLED = 1u << 0,
    GPU_TEXTURE_USAGE_COLOR_ATTACHMENT = 1u << 1,
    GPU_TEXTURE_USAGE_DEPTH_ATTACHMENT = 1u << 2,
    GPU_TEXTURE_USAGE_TRANSFER_SRC = 1u << 3,
    GPU_TEXTURE_USAGE_TRANSFER_DST = 1u << 4,
    GPU_TEXTURE_USAGE_STORAGE = 1u << 5,
};

enum GpuMemoryKind : u32 {
    // Lives on the GPU; filled with upload_buffer / upload_texture (staging).
    GPU_MEMORY_DEVICE_LOCAL = 0,
    // Persistently mapped; filled with write_buffer. For data rewritten
    // every frame.
    GPU_MEMORY_HOST_VISIBLE = 1,
};

// --- Resources -----------------------------------------------------------------

struct GpuBuffer {
    SparseId id = GPU_NULL_ID;
    u64 size = 0;
    u32 usage = 0; // GpuBufferUsage bits
    GpuMemoryKind memory = GPU_MEMORY_DEVICE_LOCAL;

    bool is_valid() const { return this->id != GPU_NULL_ID; }
};

struct GpuTexture {
    SparseId id = GPU_NULL_ID;
    GpuFormat format = GPU_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 mip_levels = 0;
    u32 usage = 0; // GpuTextureUsage bits

    bool is_valid() const { return this->id != GPU_NULL_ID; }
};

struct GpuSampler {
    SparseId id = GPU_NULL_ID;

    bool is_valid() const { return this->id != GPU_NULL_ID; }
};

struct GpuPipeline {
    SparseId id = GPU_NULL_ID;

    bool is_valid() const { return this->id != GPU_NULL_ID; }
};

// One bind group layout: which slots a group has and what goes in them
// (a descriptor set layout / a root descriptor table). Cached by the
// backend: equal descriptions give the same id.
struct GpuBindLayout {
    SparseId id = GPU_NULL_ID;

    bool is_valid() const { return this->id != GPU_NULL_ID; }
};

// Resources bound to the slots of one layout (a descriptor set / table).
struct GpuBindGroup {
    SparseId id = GPU_NULL_ID;

    bool is_valid() const { return this->id != GPU_NULL_ID; }
};

// The command list of a frame slot. Only ever obtained from a GpuFrame.
struct GpuCommandList {
    SparseId id = GPU_NULL_ID;

    bool is_valid() const { return this->id != GPU_NULL_ID; }
};

// --- Creation descriptions ------------------------------------------------------

struct GpuBufferDesc {
    u64 size = 0;
    u32 usage = 0; // GpuBufferUsage bits
    GpuMemoryKind memory = GPU_MEMORY_DEVICE_LOCAL;
};

struct GpuTextureDesc {
    GpuFormat format = GPU_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 mip_levels = 1;
    u32 usage = GPU_TEXTURE_USAGE_SAMPLED;
    // When set, the format the texture is sampled as (same texel size as
    // `format`): an sRGB attachment read back as UNORM by UI code that
    // must not decode it. UNDEFINED samples it as `format`.
    GpuFormat sampled_format = GPU_FORMAT_UNDEFINED;
};

// One mip level's pixels, tightly packed in the texture's format.
struct GpuTextureUpload {
    const void* pixels = nullptr;
    u64 size = 0;
    u32 mip_level = 0;
};

enum GpuFilter : u8 {
    GPU_FILTER_NEAREST = 0,
    GPU_FILTER_LINEAR = 1,
};

enum GpuAddressMode : u8 {
    GPU_ADDRESS_REPEAT = 0,
    GPU_ADDRESS_MIRROR = 1,
    GPU_ADDRESS_CLAMP = 2,
};

struct GpuSamplerDesc {
    GpuFilter min_filter = GPU_FILTER_LINEAR;
    GpuFilter mag_filter = GPU_FILTER_LINEAR;
    GpuFilter mip_filter = GPU_FILTER_LINEAR;
    GpuAddressMode address_u = GPU_ADDRESS_REPEAT;
    GpuAddressMode address_v = GPU_ADDRESS_REPEAT;
    GpuAddressMode address_w = GPU_ADDRESS_REPEAT;
    // 0 disables anisotropic filtering.
    f32 max_anisotropy = 0.0f;

    u64 hash() const;
};

// --- Binding -------------------------------------------------------------------

static constexpr u32 GPU_MAX_BIND_SLOTS = 24;
static constexpr u32 GPU_MAX_BIND_GROUPS = 4;
static constexpr u32 GPU_MAX_PUSH_CONSTANT_SIZE = 128;

// No combined image sampler: D3D12 and Metal have none, and Slang emits
// separate Texture2D + SamplerState for them. Shaders declare both.
enum GpuBindingType : u8 {
    GPU_BINDING_UNIFORM_BUFFER = 0,
    GPU_BINDING_STORAGE_BUFFER = 1,
    GPU_BINDING_TEXTURE = 2,
    GPU_BINDING_SAMPLER = 3,
    GPU_BINDING_STORAGE_TEXTURE = 4,
};

enum GpuShaderStage : u8 {
    GPU_STAGE_VERTEX = 1u << 0,
    GPU_STAGE_FRAGMENT = 1u << 1,
    GPU_STAGE_COMPUTE = 1u << 2,
};

struct GpuBindSlot {
    u32 slot = 0;
    GpuBindingType type = GPU_BINDING_UNIFORM_BUFFER;
    u32 count = 1;
    u8 stages = GPU_STAGE_VERTEX | GPU_STAGE_FRAGMENT; // GpuShaderStage bits
};

struct GpuBindLayoutDesc {
    GpuBindSlot slots[GPU_MAX_BIND_SLOTS] = {};
    u32 count = 0;

    bool add(u32 slot, GpuBindingType type, u8 stages, u32 count = 1);
    u64 hash() const;
};

// What to put in one slot. Which member is read follows the slot's type in
// the layout: buffers for UNIFORM / STORAGE, texture for TEXTURE /
// STORAGE_TEXTURE, sampler for SAMPLER.
struct GpuBindEntry {
    u32 slot = 0;
    GpuBuffer buffer;
    u64 offset = 0;
    u64 range = 0; // 0 = to the end of the buffer
    GpuTexture texture;
    GpuSampler sampler;
};

struct GpuBindGroupDesc {
    GpuBindEntry entries[GPU_MAX_BIND_SLOTS] = {};
    u32 count = 0;

    bool bind_buffer(u32 slot, GpuBuffer buffer, u64 offset = 0, u64 range = 0);
    bool bind_texture(u32 slot, GpuTexture texture);
    bool bind_sampler(u32 slot, GpuSampler sampler);
};

// A region of this frame's uniform ring (GPU::push_uniforms), ready for
// bind_buffer(slot, range.buffer, range.offset, range.size).
struct GpuUniformRange {
    GpuBuffer buffer;
    u64 offset = 0;
    u64 size = 0;
};

// --- Pipelines -----------------------------------------------------------------

static constexpr u32 GPU_MAX_VERTEX_BINDINGS = 8;
static constexpr u32 GPU_MAX_VERTEX_ATTRIBUTES = 16;
static constexpr u32 GPU_MAX_COLOR_ATTACHMENTS = 8;

enum GpuShaderBytecode : u8 {
    GPU_BYTECODE_SPIRV = 0,
    GPU_BYTECODE_DXIL = 1,
    GPU_BYTECODE_METALLIB = 2,
};

// One stage's code; the frontend fills it from a CompiledShader
// (shaders/compilation.hpp). The backend creates and destroys whatever
// module object its API needs inside create_pipeline.
struct GpuShaderStageDesc {
    const void* bytes = nullptr;
    usz size = 0;
    const char* entry_point = "main";
    GpuShaderBytecode kind = GPU_BYTECODE_SPIRV;

    bool is_valid() const { return this->bytes != nullptr && this->size > 0; }
};

enum GpuVertexRate : u8 {
    GPU_VERTEX_RATE_VERTEX = 0,
    GPU_VERTEX_RATE_INSTANCE = 1,
};

struct GpuVertexBinding {
    u32 stride = 0;
    GpuVertexRate rate = GPU_VERTEX_RATE_VERTEX;
};

struct GpuVertexAttribute {
    // Shader input location (VERTEX_LAYOUT in mesh_asset.hpp).
    u32 location = 0;
    u32 binding = 0;
    GpuFormat format = GPU_FORMAT_UNDEFINED;
    u32 offset = 0;
};

struct GpuVertexInput {
    GpuVertexBinding bindings[GPU_MAX_VERTEX_BINDINGS] = {};
    u32 binding_count = 0;
    GpuVertexAttribute attributes[GPU_MAX_VERTEX_ATTRIBUTES] = {};
    u32 attribute_count = 0;

    u32 add_binding(u32 stride, GpuVertexRate rate = GPU_VERTEX_RATE_VERTEX);
    bool add_attribute(u32 location, GpuFormat format, u32 offset, u32 binding = 0);
};

enum GpuTopology : u8 {
    GPU_TOPOLOGY_TRIANGLE_LIST = 0,
    GPU_TOPOLOGY_TRIANGLE_STRIP = 1,
    GPU_TOPOLOGY_LINE_LIST = 2,
    GPU_TOPOLOGY_POINT_LIST = 3,
};

enum GpuCullMode : u8 {
    GPU_CULL_NONE = 0,
    GPU_CULL_BACK = 1,
    GPU_CULL_FRONT = 2,
};

enum GpuCompare : u8 {
    GPU_COMPARE_NEVER = 0,
    GPU_COMPARE_LESS = 1,
    GPU_COMPARE_EQUAL = 2,
    GPU_COMPARE_LESS_EQUAL = 3,
    GPU_COMPARE_GREATER = 4,
    GPU_COMPARE_NOT_EQUAL = 5,
    GPU_COMPARE_GREATER_EQUAL = 6,
    GPU_COMPARE_ALWAYS = 7,
};

enum GpuBlend : u8 {
    GPU_BLEND_OPAQUE = 0,
    GPU_BLEND_ALPHA = 1,
    GPU_BLEND_ADDITIVE = 2,
};

// Fixed state defaults: triangle list, back-face culling, counter-clockwise
// front faces (right with the Y-negating projections in math/), depth LESS
// with write. Viewport and scissor are always dynamic (cmd_set_viewport).
struct GpuRasterState {
    GpuTopology topology = GPU_TOPOLOGY_TRIANGLE_LIST;
    GpuCullMode cull = GPU_CULL_BACK;
    bool front_face_clockwise = false;
    bool wireframe = false;
};

struct GpuDepthState {
    bool test = true;
    bool write = true;
    GpuCompare compare = GPU_COMPARE_LESS;
};

// What a pipeline draws into. Pipelines are tied to formats, never to a
// particular texture or render pass object; the backend makes whatever
// compatibility object its API wants (Vulkan: a template VkRenderPass).
struct GpuTargetFormats {
    GpuFormat color[GPU_MAX_COLOR_ATTACHMENTS] = {};
    u32 color_count = 0;
    GpuFormat depth = GPU_FORMAT_UNDEFINED;
    u32 samples = 1;

    u64 hash() const;
};

struct GpuPipelineDesc {
    GpuShaderStageDesc vertex;
    GpuShaderStageDesc fragment; // optional: depth-only when invalid
    GpuVertexInput vertex_input;
    GpuRasterState raster;
    GpuDepthState depth;
    GpuBlend blend = GPU_BLEND_OPAQUE;
    // Group i of this array is what cmd_bind_group(cmd, i, ...) binds.
    // Unused trailing entries stay invalid; a gap gets an empty layout.
    GpuBindLayout bind_layouts[GPU_MAX_BIND_GROUPS] = {};
    u32 bind_layout_count = 0;
    // One block, visible to every stage of the pipeline.
    u32 push_constant_size = 0;
    GpuTargetFormats targets;
    // For the backend's debug names and messages.
    const char* name = nullptr;
};

// --- Render passes -------------------------------------------------------------

enum GpuLoadOp : u8 {
    GPU_LOAD_CLEAR = 0,
    GPU_LOAD_LOAD = 1,
    GPU_LOAD_DONT_CARE = 2,
};

enum GpuStoreOp : u8 {
    GPU_STORE_STORE = 0,
    GPU_STORE_DONT_CARE = 1,
};

struct GpuColorAttachment {
    GpuTexture texture;
    GpuLoadOp load = GPU_LOAD_CLEAR;
    GpuStoreOp store = GPU_STORE_STORE;
    f32 clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct GpuDepthAttachment {
    GpuTexture texture;
    GpuLoadOp load = GPU_LOAD_CLEAR;
    GpuStoreOp store = GPU_STORE_DONT_CARE;
    f32 clear_depth = 1.0f;
    u32 clear_stencil = 0;
};

// What cmd_begin_render_pass draws into. Every attachment must already be
// in its attachment state (COLOR_ATTACHMENT / DEPTH_ATTACHMENT) and is left
// in it; the frontend issues the barriers in and out. The backend caches
// whatever object this needs (Vulkan: VkRenderPass + VkFramebuffer by
// formats, ops and textures).
struct GpuRenderPassDesc {
    GpuColorAttachment color[GPU_MAX_COLOR_ATTACHMENTS] = {};
    u32 color_count = 0;
    GpuDepthAttachment depth;
    bool has_depth = false;
    // Render area; 0 = the attachments' full extent.
    u32 width = 0;
    u32 height = 0;
    const char* name = nullptr;

    GpuTargetFormats formats() const;
};

enum GpuIndexType : u8 {
    GPU_INDEX_U16 = 0,
    GPU_INDEX_U32 = 1,
};

// --- Frames --------------------------------------------------------------------

// One frame's recording context, filled by GPU::begin_frame. `slot` indexes
// per-frame-in-flight data (uniform rings, transient bind groups, deferred
// releases); everything created with `transient_` or `push_` is valid until
// end_frame.
struct GpuFrame {
    GpuCommandList cmd;
    u32 slot = 0;
    u64 frame_index = 0;
    // The swapchain image acquired for this frame, already in
    // GPU_STATE_UNDEFINED; end_frame moves it to PRESENT.
    GpuTexture backbuffer;
};

struct GpuLimits {
    u32 max_push_constant_size = GPU_MAX_PUSH_CONSTANT_SIZE;
    u32 uniform_buffer_alignment = 256;
    u32 max_anisotropy = 1;
    u32 max_texture_size = 4096;
};

struct GpuInfo {
    GpuBackendKind backend = GPU_BACKEND_VULKAN;
    char device_name[256] = {};
    GpuLimits limits;
    GpuShaderBytecode bytecode = GPU_BYTECODE_SPIRV;
    GpuFormat swapchain_format = GPU_FORMAT_UNDEFINED;
    GpuFormat depth_format = GPU_FORMAT_UNDEFINED; // the best supported depth format
};
