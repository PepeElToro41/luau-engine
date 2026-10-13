#include "plugins/editor_viewport_plugin.hpp"

#include "engine/engine.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/plugins/render_plugin.hpp"
#include "engine/plugins/window_plugin.hpp"
#include "engine/utils/log.hpp"
#include "plugins/editor_ui_plugin.hpp"

#include <imgui_impl_vulkan.h>

bool EditorViewportPlugin::build(Engine& engine) {
    if (engine.require<WindowPlugin>() == nullptr || engine.require<RenderPlugin>() == nullptr || engine.require<EditorUiPlugin>() == nullptr) {
        return false;
    }
    if (!this->create_textures(engine, this->requested_width, this->requested_height)) {
        this->destroy_textures(engine);
        return false;
    }
    engine.add_system(engine.phases.post_update, EditorViewportPlugin::NAME, &EditorViewportPlugin::resize, this);
    engine.add_system(engine.phases.pre_render, EditorViewportPlugin::NAME, &EditorViewportPlugin::retarget, this);
    engine.add_system(engine.phases.post_render, EditorViewportPlugin::NAME, &EditorViewportPlugin::finish, this);
    engine.add_system(engine.phases.pre_shutdown, EditorViewportPlugin::NAME, &EditorViewportPlugin::detach, this);
    engine.add_system(engine.phases.shutdown, EditorViewportPlugin::NAME, &EditorViewportPlugin::shutdown, this);
    return true;
}

// --- Textures ----------------------------------------------------------------------

bool EditorViewportPlugin::create_textures(Engine& engine, const u32 width, const u32 height) {
    GpuContext* gpu = engine.gpu;
    GpuSamplerDesc sampler_desc;
    sampler_desc.mip_filter = GPU_FILTER_NEAREST;
    sampler_desc.address_u = sampler_desc.address_v = sampler_desc.address_w = GPU_ADDRESS_CLAMP;
    this->viewport_sampler = GPU::sampler(gpu, sampler_desc);

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        GpuTextureDesc desc;
        desc.format = SCENE_FORMAT;
        desc.sampled_format = UI_FORMAT;
        desc.width = width;
        desc.height = height;
        desc.usage = GPU_TEXTURE_USAGE_COLOR_ATTACHMENT | GPU_TEXTURE_USAGE_SAMPLED;
        this->viewport[i] = GPU::create_texture(gpu, desc);
        if (!this->viewport[i].is_valid()) {
            LOG::error(LOG_CATEGORY::EDITOR, "Viewport texture %ux%u could not be created", width, height);
            return false;
        }
        this->viewport_state[i] = GPU_STATE_UNDEFINED;
        this->viewport_texture[i] = ImGui_ImplVulkan_AddTexture(VK_ACCESS::sampler(gpu, this->viewport_sampler), VK_ACCESS::image_view(gpu, this->viewport[i]),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    this->width = width;
    this->height = height;
    this->requested_width = width;
    this->requested_height = height;
    return true;
}

void EditorViewportPlugin::remove_ui_textures() {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (this->viewport_texture[i] != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(this->viewport_texture[i]);
            this->viewport_texture[i] = VK_NULL_HANDLE;
        }
    }
}

void EditorViewportPlugin::destroy_textures(Engine& engine) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (this->viewport[i].is_valid() && engine.gpu != nullptr) {
            GPU::destroy_texture(engine.gpu, this->viewport[i]);
        }
        this->viewport[i] = GpuTexture{};
    }
}

// --- Systems -----------------------------------------------------------------------

void EditorViewportPlugin::resize(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    EditorViewportPlugin* self = static_cast<EditorViewportPlugin*>(ctx.user_data);
    const u32 wanted_width = self->requested_width;
    const u32 wanted_height = self->requested_height;
    if ((wanted_width == self->width && wanted_height == self->height) || wanted_width == 0 || wanted_height == 0) {
        return;
    }
    // Rare (the user is dragging a panel edge), so a full idle is acceptable.
    GPU::wait_idle(engine.gpu);
    self->remove_ui_textures();
    self->destroy_textures(engine);
    if (!self->create_textures(engine, wanted_width, wanted_height)) {
        LOG::error(LOG_CATEGORY::EDITOR, "Viewport resize to %ux%u failed", wanted_width, wanted_height);
    }
}

void EditorViewportPlugin::retarget(SystemContext& ctx) {
    EditorViewportPlugin* self = static_cast<EditorViewportPlugin*>(ctx.user_data);
    FrameContext& frame = ENGINE::frame(ctx);
    const u32 slot = frame.frame.slot;
    frame.target = self->viewport[slot];
    frame.target_state = self->viewport_state[slot];
}

void EditorViewportPlugin::finish(SystemContext& ctx) {
    EditorViewportPlugin* self = static_cast<EditorViewportPlugin*>(ctx.user_data);
    FrameContext& frame = ENGINE::frame(ctx);
    const u32 slot = frame.frame.slot;
    // Over to SAMPLED for the Viewport panel, then the backbuffer again for
    // the UI phase.
    GPU::cmd_barrier(frame.frame.cmd, self->viewport[slot], frame.target_state, GPU_STATE_SAMPLED);
    self->viewport_state[slot] = GPU_STATE_SAMPLED;
    frame.target = frame.frame.backbuffer;
    frame.target_state = GPU_STATE_UNDEFINED;
}

void EditorViewportPlugin::detach(SystemContext& ctx) {
    static_cast<EditorViewportPlugin*>(ctx.user_data)->remove_ui_textures();
}

void EditorViewportPlugin::shutdown(SystemContext& ctx) {
    static_cast<EditorViewportPlugin*>(ctx.user_data)->destroy_textures(ENGINE::of(ctx));
}
