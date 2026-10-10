#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/gpu_types.hpp"

// The backend API. Everything the renderer, the asset cache and the apps do
// with the GPU goes through these functions; nothing above this header
// names a Vulkan, D3D12 or Metal type.
//
// Which backend runs is decided once, at GPU::init, from GpuInitDesc::
// backend. The functions below forward through a table of function
// pointers that init fills (src/gpu/backend.hpp) and nothing ever refills:
// switching backends is a restart. One GpuContext per process.
//
//     GpuContext* gpu = GPU::init({GPU_BACKEND_VULKAN, window});
//     while (running) {
//         GpuFrame frame;
//         if (!GPU::begin_frame(gpu, frame)) continue;      // minimized / resizing
//         ... record into frame.cmd, drawing into frame.backbuffer ...
//         GPU::end_frame(gpu, frame);
//     }
//     GPU::wait_idle(gpu);
//     GPU::shutdown(gpu);
//
// Lifetimes. destroy_* frees now and needs an idle GPU (shutdown paths);
// release_* queues the object on the current frame slot and frees it when
// that slot's fence has been waited on (begin_frame), for objects replaced
// while frames that use them are in flight. Objects obtained from
// transient_bind_group and push_uniforms belong to the frame and are
// recycled automatically. Samplers and bind layouts are cached by hash and
// live until shutdown.

struct SDL_Window;

struct GpuInitDesc {
    GpuBackendKind backend = GPU_BACKEND_VULKAN;
    SDL_Window* window = nullptr;
    // Used if the surface supports it, else the first supported format.
    // _SRGB when the swapchain receives linear-light output, _UNORM when it
    // receives already-encoded colors (the editor's ImGui).
    GpuFormat swapchain_format = GPU_FORMAT_BGRA8_SRGB;
    bool validation = ENGINE_DEBUG != 0;
};

// The process's GPU. Plain data the frontend may read; `impl` is the
// backend's own state (src/gpu/<backend>/) and nothing else touches it.
struct GpuContext {
    GpuInfo info;
    // Current swapchain size.
    u32 width = 0;
    u32 height = 0;
    // Updated by begin_frame.
    u32 slot = 0;
    u64 frame_index = 0;
    void* impl = nullptr;
};

namespace GPU {

// --- Device and frames ---------------------------------------------------------

// Whether this build contains `kind`.
bool available(GpuBackendKind kind);
// Selects the backend, brings the device and the window's swapchain up.
// nullptr on failure (with messages). Fails if a context already exists.
GpuContext* init(const GpuInitDesc& desc);
void shutdown(GpuContext* gpu);
void wait_idle(GpuContext* gpu);
// The window changed size; the swapchain is rebuilt at the next begin_frame.
void mark_resized(GpuContext* gpu);

// Waits for the slot's previous frame, frees that slot's deferred
// releases, resets its transient pools, acquires the swapchain image and
// starts recording. False when there is nothing to draw into (minimized,
// swapchain being rebuilt); skip the frame.
bool begin_frame(GpuContext* gpu, GpuFrame& out);
// Ends recording, moves the backbuffer to PRESENT, submits and presents.
bool end_frame(GpuContext* gpu, GpuFrame& frame);

// --- Resources -----------------------------------------------------------------

// `data` (optional) is uploaded or written before returning. An invalid
// handle on failure, with a message.
GpuBuffer create_buffer(GpuContext* gpu, const GpuBufferDesc& desc, const void* data = nullptr);
// HOST_VISIBLE buffers: memcpy into the mapping.
bool write_buffer(GpuContext* gpu, GpuBuffer buffer, const void* data, u64 size, u64 offset = 0);
// DEVICE_LOCAL buffers: staged copy, synchronous on the graphics queue.
bool upload_buffer(GpuContext* gpu, GpuBuffer buffer, const void* data, u64 size, u64 offset = 0);
void destroy_buffer(GpuContext* gpu, GpuBuffer buffer);
void release_buffer(GpuContext* gpu, GpuBuffer buffer);

// 2D, `mip_levels` levels, one view over all of them. Created in
// GPU_STATE_UNDEFINED.
GpuTexture create_texture(GpuContext* gpu, const GpuTextureDesc& desc);
// Staged copy of each mip, synchronous; leaves the texture in
// GPU_STATE_SAMPLED.
bool upload_texture(GpuContext* gpu, GpuTexture texture, const GpuTextureUpload* mips, u32 count);
void destroy_texture(GpuContext* gpu, GpuTexture texture);
void release_texture(GpuContext* gpu, GpuTexture texture);

// Cached: equal descriptions return the same sampler.
GpuSampler sampler(GpuContext* gpu, const GpuSamplerDesc& desc);

// --- Binding -------------------------------------------------------------------

// Cached: equal descriptions return the same layout. An empty desc is the
// empty layout, for gaps in a pipeline's groups.
GpuBindLayout bind_layout(GpuContext* gpu, const GpuBindLayoutDesc& desc);

// Allocates a group for `layout` and writes `desc` into it in one go.
// Long-lived: freed with release_bind_group.
GpuBindGroup create_bind_group(GpuContext* gpu, GpuBindLayout layout, const GpuBindGroupDesc& desc);
void release_bind_group(GpuContext* gpu, GpuBindGroup group);
// Same, from the frame slot's pool: valid until end_frame, recycled when
// the slot comes around. For per-frame and per-material-per-frame groups.
GpuBindGroup transient_bind_group(GpuContext* gpu, GpuBindLayout layout, const GpuBindGroupDesc& desc);

// Copies `size` bytes into the frame slot's uniform ring (aligned to
// info.limits.uniform_buffer_alignment) and returns where they landed, to
// bind_buffer into a group. Valid until end_frame. An empty range when
// the ring is full (with a message).
GpuUniformRange push_uniforms(GpuContext* gpu, const void* data, u32 size);

// --- Pipelines -----------------------------------------------------------------

// Builds the pipeline and its layout from the desc. The shader modules
// it needs are created and destroyed inside. An invalid handle (with the
// backend's message) on failure.
GpuPipeline create_pipeline(GpuContext* gpu, const GpuPipelineDesc& desc);
void destroy_pipeline(GpuContext* gpu, GpuPipeline pipeline);
void release_pipeline(GpuContext* gpu, GpuPipeline pipeline);

// --- Commands ------------------------------------------------------------------
// All on the frame's command list, between begin_frame and end_frame.

// Moves `texture` from `from` to `to`, with the fences the two states
// need. `from` UNDEFINED discards the contents.
void cmd_barrier(GpuCommandList cmd, GpuTexture texture, GpuResourceState from, GpuResourceState to);

void cmd_begin_render_pass(GpuCommandList cmd, const GpuRenderPassDesc& desc);
void cmd_end_render_pass(GpuCommandList cmd);

void cmd_bind_pipeline(GpuCommandList cmd, GpuPipeline pipeline);
// `index` is the position in GpuPipelineDesc::bind_layouts of the pipeline
// currently bound.
void cmd_bind_group(GpuCommandList cmd, u32 index, GpuBindGroup group);
void cmd_push_constants(GpuCommandList cmd, const void* data, u32 size);
void cmd_bind_vertex_buffer(GpuCommandList cmd, u32 binding, GpuBuffer buffer, u64 offset = 0);
void cmd_bind_index_buffer(GpuCommandList cmd, GpuBuffer buffer, GpuIndexType type, u64 offset = 0);
// Full-target viewport and scissor; call after begin_render_pass.
void cmd_set_viewport(GpuCommandList cmd, u32 width, u32 height);
void cmd_draw(GpuCommandList cmd, u32 vertex_count, u32 first_vertex = 0, u32 instance_count = 1);
void cmd_draw_indexed(GpuCommandList cmd, u32 index_count, u32 first_index = 0, i32 vertex_offset = 0, u32 instance_count = 1);

// Outside a render pass. The texture must be in TRANSFER_DST (clear, blit
// dst) or TRANSFER_SRC (blit src).
void cmd_clear_color(GpuCommandList cmd, GpuTexture texture, const f32 rgba[4]);
void cmd_clear_depth(GpuCommandList cmd, GpuTexture texture, f32 depth, u32 stencil);
void cmd_blit(GpuCommandList cmd, GpuTexture src, GpuTexture dst, bool linear);

// Debug regions (validation layers, RenderDoc); no-ops when unsupported.
void cmd_begin_label(GpuCommandList cmd, const char* name);
void cmd_end_label(GpuCommandList cmd);

// --- Messages ------------------------------------------------------------------

enum LogLevel : u32 {
    LOG_INFO = 0,
    LOG_WARNING = 1,
    LOG_ERROR = 2,
};
using LogFn = void (*)(LogLevel level, const char* text, void* user_data);
// Where the backend's messages go; stderr when unset. The renderer chains
// its own sink to it.
void set_log_sink(LogFn fn, void* user_data);

} // namespace GPU
