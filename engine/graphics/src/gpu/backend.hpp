#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/gpu.hpp"

// Private to src/gpu/. The table every GPU:: function forwards through: one
// pointer per function, filled once by GPU::init from the backend it
// selected (VULKAN_BACKEND::table(), later D3D12_BACKEND::table()). A
// backend defines its functions with these exact signatures and returns a
// static table pointing at them.

struct GpuBackend {
    bool (*init)(GpuContext* gpu, const GpuInitDesc& desc);
    void (*shutdown)(GpuContext* gpu);
    void (*wait_idle)(GpuContext* gpu);
    void (*mark_resized)(GpuContext* gpu);
    bool (*begin_frame)(GpuContext* gpu, GpuFrame& out);
    bool (*end_frame)(GpuContext* gpu, GpuFrame& frame);

    GpuBuffer (*create_buffer)(GpuContext* gpu, const GpuBufferDesc& desc, const void* data);
    bool (*write_buffer)(GpuContext* gpu, GpuBuffer buffer, const void* data, u64 size, u64 offset);
    bool (*upload_buffer)(GpuContext* gpu, GpuBuffer buffer, const void* data, u64 size, u64 offset);
    void (*destroy_buffer)(GpuContext* gpu, GpuBuffer buffer);
    void (*release_buffer)(GpuContext* gpu, GpuBuffer buffer);

    GpuTexture (*create_texture)(GpuContext* gpu, const GpuTextureDesc& desc);
    bool (*upload_texture)(GpuContext* gpu, GpuTexture texture, const GpuTextureUpload* mips, u32 count);
    void (*destroy_texture)(GpuContext* gpu, GpuTexture texture);
    void (*release_texture)(GpuContext* gpu, GpuTexture texture);

    GpuSampler (*sampler)(GpuContext* gpu, const GpuSamplerDesc& desc);

    GpuBindLayout (*bind_layout)(GpuContext* gpu, const GpuBindLayoutDesc& desc);
    GpuBindGroup (*create_bind_group)(GpuContext* gpu, GpuBindLayout layout, const GpuBindGroupDesc& desc);
    void (*release_bind_group)(GpuContext* gpu, GpuBindGroup group);
    GpuBindGroup (*transient_bind_group)(GpuContext* gpu, GpuBindLayout layout, const GpuBindGroupDesc& desc);
    GpuUniformRange (*push_uniforms)(GpuContext* gpu, const void* data, u32 size);

    GpuPipeline (*create_pipeline)(GpuContext* gpu, const GpuPipelineDesc& desc);
    void (*destroy_pipeline)(GpuContext* gpu, GpuPipeline pipeline);
    void (*release_pipeline)(GpuContext* gpu, GpuPipeline pipeline);

    void (*cmd_barrier)(GpuCommandList cmd, GpuTexture texture, GpuResourceState from, GpuResourceState to);
    void (*cmd_begin_render_pass)(GpuCommandList cmd, const GpuRenderPassDesc& desc);
    void (*cmd_end_render_pass)(GpuCommandList cmd);
    void (*cmd_bind_pipeline)(GpuCommandList cmd, GpuPipeline pipeline);
    void (*cmd_bind_group)(GpuCommandList cmd, u32 index, GpuBindGroup group);
    void (*cmd_push_constants)(GpuCommandList cmd, const void* data, u32 size);
    void (*cmd_bind_vertex_buffer)(GpuCommandList cmd, u32 binding, GpuBuffer buffer, u64 offset);
    void (*cmd_bind_index_buffer)(GpuCommandList cmd, GpuBuffer buffer, GpuIndexType type, u64 offset);
    void (*cmd_set_viewport)(GpuCommandList cmd, u32 width, u32 height);
    void (*cmd_draw)(GpuCommandList cmd, u32 vertex_count, u32 first_vertex, u32 instance_count);
    void (*cmd_draw_indexed)(GpuCommandList cmd, u32 index_count, u32 first_index, i32 vertex_offset, u32 instance_count);
    void (*cmd_clear_color)(GpuCommandList cmd, GpuTexture texture, const f32 rgba[4]);
    void (*cmd_clear_depth)(GpuCommandList cmd, GpuTexture texture, f32 depth, u32 stencil);
    void (*cmd_blit)(GpuCommandList cmd, GpuTexture src, GpuTexture dst, bool linear);
    void (*cmd_begin_label)(GpuCommandList cmd, const char* name);
    void (*cmd_end_label)(GpuCommandList cmd);
};

namespace VULKAN_BACKEND {
const GpuBackend* table();
} // namespace VULKAN_BACKEND

namespace GPU {
// Formats and sends a message to the log sink (stderr when unset).
void log(LogLevel level, const char* format, ...);
} // namespace GPU
