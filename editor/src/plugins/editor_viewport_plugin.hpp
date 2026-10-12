#pragma once

#include "engine/gpu/gpu_types.hpp"
#include "engine/gpu/vulkan/vk_access.hpp"
#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

// The per-slot textures the engine renders the scene into for the
// Viewport panel, and the two render-pipeline systems that route the
// frame through them: "pre_render" points the FrameContext at this slot's
// texture (with the state it was left in), "post_render" barriers it to
// SAMPLED for ImGui and points the context back at the backbuffer, in
// UNDEFINED, for the UI phase to draw into. The Viewport panel reads
// `viewport_texture[slot]` to show it and writes `requested_width` /
// `requested_height`; the "post_update" system recreates the textures at
// that size (a GPU idle, rare: the user is dragging a panel edge) before
// the next frame begins.
//
// ImGui writes already-encoded colors, so the swapchain is UNORM
// (UI_FORMAT, the window plugin's swapchain format); the scene is drawn
// into an sRGB texture (SCENE_FORMAT) that ImGui samples through a UNORM
// view, copying the encoded bytes through untouched.
//
// Teardown: "pre_shutdown" removes the ImGui textures while the UI plugin's
// backend is alive, "shutdown" destroys the GPU textures. Requires
// WindowPlugin, RenderPlugin and EditorUiPlugin (the ImGui Vulkan backend).
struct EditorViewportPlugin : Plugin {
    static constexpr const char* NAME = "editor_viewport";
    static constexpr GpuFormat UI_FORMAT = GPU_FORMAT_BGRA8_UNORM;
    static constexpr GpuFormat SCENE_FORMAT = GPU_FORMAT_BGRA8_SRGB;

    bool build(Engine& engine) override;

    GpuTexture viewport[FRAMES_IN_FLIGHT] = {};
    GpuResourceState viewport_state[FRAMES_IN_FLIGHT] = {};
    VkDescriptorSet viewport_texture[FRAMES_IN_FLIGHT] = {};
    GpuSampler viewport_sampler;
    u32 width = 1280;
    u32 height = 720;
    u32 requested_width = 1280;
    u32 requested_height = 720;

private:
    bool create_textures(Engine& engine, u32 width, u32 height);
    void remove_ui_textures();
    void destroy_textures(Engine& engine);

    static void resize(SystemContext& ctx);
    static void retarget(SystemContext& ctx);
    static void finish(SystemContext& ctx);
    static void detach(SystemContext& ctx);
    static void shutdown(SystemContext& ctx);
};
