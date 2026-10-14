#include "engine/render/renderer.hpp"

#include "engine/ecs/world.hpp"
#include "engine/utils/hash.hpp"
#include "engine/utils/log.hpp"

#include <cstring>
#include <utility>

// --- Lifetime ----------------------------------------------------------------------

static bool init_frame_set(RenderResources& resources) {
    // Set 0: the frame block at binding 0 and the sampler table at 1..4, as
    // engine/frame.slang declares them. Shaders may use any subset.
    GpuBindLayoutDesc layout;
    layout.add(0, GPU_BINDING_UNIFORM_BUFFER, GPU_STAGE_VERTEX | GPU_STAGE_FRAGMENT);
    for (u32 i = 0; i < RENDERER_SAMPLER_COUNT; ++i) {
        layout.add(1 + i, GPU_BINDING_SAMPLER, GPU_STAGE_VERTEX | GPU_STAGE_FRAGMENT);
    }
    resources.frame_layout = GPU::bind_layout(resources.gpu, layout);
    if (!resources.frame_layout.is_valid()) {
        return false;
    }

    ShaderReflection& interface = resources.frame_interface;
    interface = ShaderReflection{};
    ReflectedBinding& block = interface.bindings[interface.binding_count++];
    block.set = 0;
    block.binding = 0;
    block.type = GPU_BINDING_UNIFORM_BUFFER;
    block.count = 1;
    block.stages = SHADER_STAGE_MASK_VERTEX | SHADER_STAGE_MASK_FRAGMENT;
    strcpy(block.name, "frame");
    block.block_size = sizeof(FrameUniforms);
    static const char* const sampler_names[RENDERER_SAMPLER_COUNT] = {"sampler_linear", "sampler_linear_clamp", "sampler_nearest", "sampler_nearest_clamp"};
    for (u32 i = 0; i < RENDERER_SAMPLER_COUNT; ++i) {
        ReflectedBinding& sampler = interface.bindings[interface.binding_count++];
        sampler.set = 0;
        sampler.binding = 1 + i;
        sampler.type = GPU_BINDING_SAMPLER;
        sampler.count = 1;
        sampler.stages = SHADER_STAGE_MASK_VERTEX | SHADER_STAGE_MASK_FRAGMENT;
        strcpy(sampler.name, sampler_names[i]);
    }

    GpuSamplerDesc desc;
    resources.samplers[RENDERER_SAMPLER_LINEAR] = GPU::sampler(resources.gpu, desc);
    desc.address_u = desc.address_v = desc.address_w = GPU_ADDRESS_CLAMP;
    resources.samplers[RENDERER_SAMPLER_LINEAR_CLAMP] = GPU::sampler(resources.gpu, desc);
    desc.min_filter = desc.mag_filter = desc.mip_filter = GPU_FILTER_NEAREST;
    resources.samplers[RENDERER_SAMPLER_NEAREST_CLAMP] = GPU::sampler(resources.gpu, desc);
    desc.address_u = desc.address_v = desc.address_w = GPU_ADDRESS_REPEAT;
    resources.samplers[RENDERER_SAMPLER_NEAREST] = GPU::sampler(resources.gpu, desc);
    for (const GpuSampler sampler : resources.samplers) {
        if (!sampler.is_valid()) {
            return false;
        }
    }

    // What unset texture slots sample.
    GpuTextureDesc white_desc;
    white_desc.format = GPU_FORMAT_RGBA8_UNORM;
    white_desc.width = 1;
    white_desc.height = 1;
    resources.white = GPU::create_texture(resources.gpu, white_desc);
    if (!resources.white.is_valid()) {
        return false;
    }
    const u8 white_pixel[4] = {255, 255, 255, 255};
    GpuTextureUpload upload;
    upload.pixels = white_pixel;
    upload.size = sizeof(white_pixel);
    return GPU::upload_texture(resources.gpu, resources.white, &upload, 1);
}

bool RENDER_RESOURCES::init(RenderResources& resources, GpuContext* gpu, AssetResourceProvider* provider, const char* engine_render_dir) {
    if (gpu == nullptr) {
        return false;
    }
    resources.gpu = gpu;
    resources.provider = provider;
    if (engine_render_dir != nullptr) {
        strncpy(resources.engine_render_dir, engine_render_dir, RENDERER_PATH_MAX - 1);
    }
    GPU_ASSETS::init(resources.assets, gpu, provider);
    return init_frame_set(resources);
}

void RENDER_RESOURCES::shutdown(RenderResources& resources) {
    if (resources.gpu == nullptr) {
        return;
    }
    PIPELINES::free(resources);
    GPU_ASSETS::shutdown(resources.assets);
    if (resources.white.is_valid()) {
        GPU::destroy_texture(resources.gpu, resources.white);
        resources.white = GpuTexture{};
    }
    resources.gpu = nullptr;
    resources.provider = nullptr;
}

void RENDER_RESOURCES::set_project_render_dir(RenderResources& resources, const char* path) {
    resources.project_render_dir[0] = '\0';
    if (path != nullptr) {
        strncpy(resources.project_render_dir, path, RENDERER_PATH_MAX - 1);
    }
}

bool RENDERER::init(Renderer& renderer, RenderResources* resources, World* world, AssetResourceProvider* provider, BaseAllocator* frame_allocator) {
    if (resources == nullptr || resources->gpu == nullptr || frame_allocator == nullptr) {
        return false;
    }
    renderer.gpu = resources->gpu;
    renderer.resources = resources;
    renderer.world = world;
    renderer.provider = provider;
    renderer.frame_allocator = frame_allocator;
    renderer.graph.init();
    renderer.plan.init();
    renderer.next_plan.init();
    build_default_graph(renderer);
    return true;
}

void RENDERER::shutdown(Renderer& renderer, const bool gpu_idle) {
    if (renderer.gpu == nullptr) {
        return;
    }
    if (gpu_idle) {
        GRAPH_EXECUTOR::free(renderer);
    } else {
        GRAPH_EXECUTOR::release(renderer);
    }
    renderer.material_groups.free();
    renderer.materials.free();
    renderer.plan.free();
    renderer.next_plan.free();
    renderer.graph.free();
    renderer.diagnostics.free();
    renderer.custom_passes.free();
    renderer.plan_valid = false;
    renderer.gpu = nullptr;
    renderer.world = nullptr;
    renderer.provider = nullptr;
    renderer.resources = nullptr;
    renderer.frame_allocator = nullptr;
}

// --- Graph -----------------------------------------------------------------------

void RENDERER::build_default_graph(Renderer& renderer) {
    RenderGraph& graph = renderer.graph;
    graph.init(graph.allocator);
    renderer.default_forward = graph.add_pass("forward", RENDER_PASS_DRAW_SCENE);
    graph.set_draw_scene_tag(renderer.default_forward, "forward");
    graph.set_color_attachment(renderer.default_forward, 0, graph.backbuffer, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
    graph.set_clear_color(renderer.default_forward, 0, renderer.clear_color);
    graph.set_depth_attachment(renderer.default_forward, graph.backbuffer_depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
}

bool RENDERER::register_custom_pass(Renderer& renderer, const char* name, const CustomPassFn fn, void* user_data) {
    if (name == nullptr || name[0] == '\0' || fn == nullptr) {
        return false;
    }
    CustomPass entry;
    entry.fn = fn;
    entry.user_data = user_data;
    renderer.custom_passes.insert(HASH::fnv1a_str(name), entry);
    return true;
}

bool RENDERER::unregister_custom_pass(Renderer& renderer, const char* name) {
    return name != nullptr && renderer.custom_passes.remove(HASH::fnv1a_str(name));
}

static void report_diagnostics(Renderer& renderer) {
    if (renderer.reported_version == renderer.graph.version) {
        return;
    }
    renderer.reported_version = renderer.graph.version;
    for (const RenderDiagnostic& diagnostic : renderer.diagnostics) {
        const RenderPassDesc* pass = renderer.graph.pass(diagnostic.pass);
        const RenderResourceDesc* resource = renderer.graph.resource(diagnostic.resource);
        LOG::write(diagnostic.severity == RENDER_DIAG_ERROR ? LOG_ERROR : LOG_WARNING, LOG_CATEGORY::RENDER, "render graph: %s%s%s%s%s%s%s",
            pass != nullptr ? "pass '" : "", pass != nullptr ? pass->name : "", pass != nullptr ? "': " : "", diagnostic.message, resource != nullptr ? " (resource '" : "",
            resource != nullptr ? resource->name : "", resource != nullptr ? "')" : "");
    }
}

static bool recompile(Renderer& renderer, const RenderBackbufferInfo& info) {
    renderer.diagnostics.clear();
    const bool compiled = RENDER_GRAPH::compile(renderer.graph, info, renderer.next_plan, &renderer.diagnostics);
    report_diagnostics(renderer);
    if (!compiled) {
        return false;
    }
    if (!GRAPH_EXECUTOR::realize(renderer, renderer.next_plan)) {
        LOG::error(LOG_CATEGORY::RENDER, "render graph: could not realize the plan; keeping the previous one");
        return false;
    }
    std::swap(renderer.plan, renderer.next_plan);
    renderer.plan_valid = true;
    renderer.last_backbuffer = info;
    return true;
}

// --- Frame -----------------------------------------------------------------------

// The renderer's own pick, else the camera tagged RenderCamera, else the
// first camera there is, else 0.
static EntityId find_render_camera(const Renderer& renderer, World* world) {
    if (renderer.camera != 0 && world->alive(renderer.camera) && world->has<Transform>(renderer.camera) && world->has<Camera>(renderer.camera)) {
        return renderer.camera;
    }
    const EntityId tagged = world->query<Transform, Camera>().with<RenderCamera>().first();
    if (tagged != 0) {
        return tagged;
    }
    return world->query<Transform, Camera>().first();
}

static void write_frame_set(Renderer& renderer, const u32 width, const u32 height) {
    FrameUniforms& uniforms = renderer.frame_uniforms;
    Vector3 eye(0.0f, 1.0f, 3.0f);
    uniforms.view = Matrix4x4::look_at(eye, Vector3::zero(), Vector3::up());
    f32 fov_y = MATH::radians(60.0f);
    f32 near_plane = 0.1f;
    f32 far_plane = 1000.0f;
    if (renderer.world != nullptr) {
        const EntityId camera_entity = find_render_camera(renderer, renderer.world);
        if (camera_entity != 0) {
            const Transform* transform = renderer.world->get<Transform>(camera_entity);
            const Camera* camera = renderer.world->get<Camera>(camera_entity);
            eye = transform->position;
            uniforms.view = transform->matrix().inverse_affine();
            fov_y = camera->fov_y;
            near_plane = camera->near_plane;
            far_plane = camera->far_plane;
        }
    }
    const f32 aspect = height > 0 ? static_cast<f32>(width) / static_cast<f32>(height) : 1.0f;
    uniforms.projection = Matrix4x4::perspective(fov_y, aspect, near_plane, far_plane);
    uniforms.view_projection = uniforms.projection * uniforms.view;
    uniforms.camera_position = Vector4(eye, 1.0f);
    uniforms.time = Vector4(renderer.time, renderer.delta_time, 0.0f, 0.0f);

    const GpuUniformRange range = GPU::push_uniforms(renderer.gpu, &uniforms, sizeof(uniforms));
    GpuBindGroupDesc desc;
    desc.bind_buffer(0, range.buffer, range.offset, range.size);
    for (u32 i = 0; i < RENDERER_SAMPLER_COUNT; ++i) {
        desc.bind_sampler(1 + i, renderer.resources->samplers[i]);
    }
    renderer.frame_group = GPU::transient_bind_group(renderer.gpu, renderer.resources->frame_layout, desc);
}

static GpuResourceState render_fallback(Renderer& renderer, const FrameContext& frame) {
    // No plan: clear the target so the window shows something sane.
    GPU::cmd_barrier(frame.frame.cmd, frame.target, frame.target_state, GPU_STATE_COLOR_ATTACHMENT);
    GpuRenderPassDesc pass;
    pass.color[0].texture = frame.target;
    for (u32 c = 0; c < 4; ++c) {
        pass.color[0].clear[c] = renderer.clear_color[c];
    }
    pass.color_count = 1;
    pass.name = "fallback clear";
    GPU::cmd_begin_render_pass(frame.frame.cmd, pass);
    GPU::cmd_end_render_pass(frame.frame.cmd);
    return GPU_STATE_COLOR_ATTACHMENT;
}

GpuResourceState RENDERER::render(Renderer& renderer, const FrameContext& frame) {
    renderer.material_groups.clear();
    write_frame_set(renderer, frame.target.width, frame.target.height);

    RenderBackbufferInfo info;
    info.color_format = frame.target.format;
    info.depth_format = renderer.gpu->info.depth_format;
    info.width = frame.target.width;
    info.height = frame.target.height;

    if (renderer.graph.dirty || !renderer.plan_valid || info != renderer.last_backbuffer) {
        if (!recompile(renderer, info) && info != renderer.last_backbuffer) {
            // A plan for another target cannot run: its images were sized
            // for the old one.
            renderer.plan_valid = false;
        }
        renderer.graph.dirty = false;
    }
    if (!renderer.plan_valid) {
        return render_fallback(renderer, frame);
    }
    return GRAPH_EXECUTOR::execute(renderer, frame, renderer.plan);
}
