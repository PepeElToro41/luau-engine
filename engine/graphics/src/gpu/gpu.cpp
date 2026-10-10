#include "engine/gpu/gpu.hpp"

#include "gpu/backend.hpp"

#include <cstdarg>
#include <cstdio>

// The selected backend. Set once by GPU::init, never changed afterwards.
static const GpuBackend* g_backend = nullptr;
static GpuContext g_context;
static bool g_context_alive = false;

static GPU::LogFn g_log_fn = nullptr;
static void* g_log_user_data = nullptr;

// --- Messages ------------------------------------------------------------------

void GPU::set_log_sink(const LogFn fn, void* user_data) {
    g_log_fn = fn;
    g_log_user_data = user_data;
}

void GPU::log(const LogLevel level, const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (g_log_fn != nullptr) {
        g_log_fn(level, text, g_log_user_data);
        return;
    }
    static const char* const prefixes[] = {"", "warning: ", "error: "};
    fprintf(stderr, "[gpu] %s%s\n", prefixes[level], text);
}

// --- Device and frames ---------------------------------------------------------

static const GpuBackend* backend_for(const GpuBackendKind kind) {
    switch (kind) {
    case GPU_BACKEND_VULKAN:
        return VULKAN_BACKEND::table();
    case GPU_BACKEND_D3D12:
    case GPU_BACKEND_METAL:
        break;
    }
    return nullptr;
}

bool GPU::available(const GpuBackendKind kind) {
    return backend_for(kind) != nullptr;
}

GpuContext* GPU::init(const GpuInitDesc& desc) {
    if (g_context_alive) {
        log(LOG_ERROR, "init: a GPU context already exists; switching backends needs a restart");
        return nullptr;
    }
    const GpuBackend* backend = backend_for(desc.backend);
    if (backend == nullptr) {
        log(LOG_ERROR, "init: backend %u is not part of this build", static_cast<unsigned>(desc.backend));
        return nullptr;
    }
    if (g_backend != nullptr && g_backend != backend) {
        log(LOG_ERROR, "init: the backend was already selected; switching needs a restart");
        return nullptr;
    }
    g_backend = backend;
    g_context = GpuContext{};
    g_context.info.backend = desc.backend;
    if (!backend->init(&g_context, desc)) {
        backend->shutdown(&g_context);
        g_context = GpuContext{};
        return nullptr;
    }
    g_context_alive = true;
    return &g_context;
}

void GPU::shutdown(GpuContext* gpu) {
    if (gpu == nullptr || !g_context_alive) {
        return;
    }
    g_backend->shutdown(gpu);
    g_context = GpuContext{};
    g_context_alive = false;
}

void GPU::wait_idle(GpuContext* gpu) { g_backend->wait_idle(gpu); }
void GPU::mark_resized(GpuContext* gpu) { g_backend->mark_resized(gpu); }
bool GPU::begin_frame(GpuContext* gpu, GpuFrame& out) { return g_backend->begin_frame(gpu, out); }
bool GPU::end_frame(GpuContext* gpu, GpuFrame& frame) { return g_backend->end_frame(gpu, frame); }

// --- Resources -----------------------------------------------------------------

GpuBuffer GPU::create_buffer(GpuContext* gpu, const GpuBufferDesc& desc, const void* data) { return g_backend->create_buffer(gpu, desc, data); }
bool GPU::write_buffer(GpuContext* gpu, const GpuBuffer buffer, const void* data, const u64 size, const u64 offset) {
    return g_backend->write_buffer(gpu, buffer, data, size, offset);
}
bool GPU::upload_buffer(GpuContext* gpu, const GpuBuffer buffer, const void* data, const u64 size, const u64 offset) {
    return g_backend->upload_buffer(gpu, buffer, data, size, offset);
}
void GPU::destroy_buffer(GpuContext* gpu, const GpuBuffer buffer) { g_backend->destroy_buffer(gpu, buffer); }
void GPU::release_buffer(GpuContext* gpu, const GpuBuffer buffer) { g_backend->release_buffer(gpu, buffer); }

GpuTexture GPU::create_texture(GpuContext* gpu, const GpuTextureDesc& desc) { return g_backend->create_texture(gpu, desc); }
bool GPU::upload_texture(GpuContext* gpu, const GpuTexture texture, const GpuTextureUpload* mips, const u32 count) {
    return g_backend->upload_texture(gpu, texture, mips, count);
}
void GPU::destroy_texture(GpuContext* gpu, const GpuTexture texture) { g_backend->destroy_texture(gpu, texture); }
void GPU::release_texture(GpuContext* gpu, const GpuTexture texture) { g_backend->release_texture(gpu, texture); }

GpuSampler GPU::sampler(GpuContext* gpu, const GpuSamplerDesc& desc) { return g_backend->sampler(gpu, desc); }

// --- Binding -------------------------------------------------------------------

GpuBindLayout GPU::bind_layout(GpuContext* gpu, const GpuBindLayoutDesc& desc) { return g_backend->bind_layout(gpu, desc); }
GpuBindGroup GPU::create_bind_group(GpuContext* gpu, const GpuBindLayout layout, const GpuBindGroupDesc& desc) {
    return g_backend->create_bind_group(gpu, layout, desc);
}
void GPU::release_bind_group(GpuContext* gpu, const GpuBindGroup group) { g_backend->release_bind_group(gpu, group); }
GpuBindGroup GPU::transient_bind_group(GpuContext* gpu, const GpuBindLayout layout, const GpuBindGroupDesc& desc) {
    return g_backend->transient_bind_group(gpu, layout, desc);
}
GpuUniformRange GPU::push_uniforms(GpuContext* gpu, const void* data, const u32 size) { return g_backend->push_uniforms(gpu, data, size); }

// --- Pipelines -----------------------------------------------------------------

GpuPipeline GPU::create_pipeline(GpuContext* gpu, const GpuPipelineDesc& desc) { return g_backend->create_pipeline(gpu, desc); }
void GPU::destroy_pipeline(GpuContext* gpu, const GpuPipeline pipeline) { g_backend->destroy_pipeline(gpu, pipeline); }
void GPU::release_pipeline(GpuContext* gpu, const GpuPipeline pipeline) { g_backend->release_pipeline(gpu, pipeline); }

// --- Commands ------------------------------------------------------------------

void GPU::cmd_barrier(const GpuCommandList cmd, const GpuTexture texture, const GpuResourceState from, const GpuResourceState to) {
    g_backend->cmd_barrier(cmd, texture, from, to);
}
void GPU::cmd_begin_render_pass(const GpuCommandList cmd, const GpuRenderPassDesc& desc) { g_backend->cmd_begin_render_pass(cmd, desc); }
void GPU::cmd_end_render_pass(const GpuCommandList cmd) { g_backend->cmd_end_render_pass(cmd); }
void GPU::cmd_bind_pipeline(const GpuCommandList cmd, const GpuPipeline pipeline) { g_backend->cmd_bind_pipeline(cmd, pipeline); }
void GPU::cmd_bind_group(const GpuCommandList cmd, const u32 index, const GpuBindGroup group) { g_backend->cmd_bind_group(cmd, index, group); }
void GPU::cmd_push_constants(const GpuCommandList cmd, const void* data, const u32 size) { g_backend->cmd_push_constants(cmd, data, size); }
void GPU::cmd_bind_vertex_buffer(const GpuCommandList cmd, const u32 binding, const GpuBuffer buffer, const u64 offset) {
    g_backend->cmd_bind_vertex_buffer(cmd, binding, buffer, offset);
}
void GPU::cmd_bind_index_buffer(const GpuCommandList cmd, const GpuBuffer buffer, const GpuIndexType type, const u64 offset) {
    g_backend->cmd_bind_index_buffer(cmd, buffer, type, offset);
}
void GPU::cmd_set_viewport(const GpuCommandList cmd, const u32 width, const u32 height) { g_backend->cmd_set_viewport(cmd, width, height); }
void GPU::cmd_draw(const GpuCommandList cmd, const u32 vertex_count, const u32 first_vertex, const u32 instance_count) {
    g_backend->cmd_draw(cmd, vertex_count, first_vertex, instance_count);
}
void GPU::cmd_draw_indexed(const GpuCommandList cmd, const u32 index_count, const u32 first_index, const i32 vertex_offset, const u32 instance_count) {
    g_backend->cmd_draw_indexed(cmd, index_count, first_index, vertex_offset, instance_count);
}
void GPU::cmd_clear_color(const GpuCommandList cmd, const GpuTexture texture, const f32 rgba[4]) { g_backend->cmd_clear_color(cmd, texture, rgba); }
void GPU::cmd_clear_depth(const GpuCommandList cmd, const GpuTexture texture, const f32 depth, const u32 stencil) {
    g_backend->cmd_clear_depth(cmd, texture, depth, stencil);
}
void GPU::cmd_blit(const GpuCommandList cmd, const GpuTexture src, const GpuTexture dst, const bool linear) { g_backend->cmd_blit(cmd, src, dst, linear); }
void GPU::cmd_begin_label(const GpuCommandList cmd, const char* name) { g_backend->cmd_begin_label(cmd, name); }
void GPU::cmd_end_label(const GpuCommandList cmd) { g_backend->cmd_end_label(cmd); }
