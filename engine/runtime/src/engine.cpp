#include "engine/engine.h"

bool Engine::init(GpuDevice* gpu) {
    this->gpu = gpu;
    this->time = 0.0;
    if (this->create_singleton<AssetResourceProvider>() == nullptr) {
        return false;
    }
    GpuResourceManager* resources = this->create_singleton<GpuResourceManager>();
    if (resources == nullptr || !resources->init(gpu)) {
        return false;
    }
    return true;
}

void Engine::shutdown() {
    // Singletons that own memory release it before the store destroys them.
    if (AssetResourceProvider* assets = this->get_singleton<AssetResourceProvider>()) {
        assets->free();
    }
    if (GpuResourceManager* resources = this->get_singleton<GpuResourceManager>()) {
        resources->shutdown();
    }
    this->singletons.free();
    this->gpu = nullptr;
}

void Engine::update(const f32 dt) {
    this->time += dt;
}

void Engine::render(const FrameContext& frame) {
    // frame.slot's fence was waited on by FrameScheduler::begin, so whatever
    // was released when this slot was last current can go now.
    if (GpuResourceManager* resources = this->get_singleton<GpuResourceManager>()) {
        resources->begin_frame(frame.slot);
    }

    VkClearValue clear[RENDER_TARGET_ATTACHMENT_COUNT];
    render_target_clear_values(this->clear_color, clear);

    VkRenderPassBeginInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_info.renderPass = frame.target.render_pass;
    pass_info.framebuffer = frame.target.framebuffer;
    pass_info.renderArea.extent = frame.target.extent;
    pass_info.clearValueCount = RENDER_TARGET_ATTACHMENT_COUNT;
    pass_info.pClearValues = clear;

    vkCmdBeginRenderPass(frame.cmd, &pass_info, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(frame.target.extent.width);
    viewport.height = static_cast<float>(frame.target.extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = frame.target.extent;
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

    // Scene drawing goes here.

    vkCmdEndRenderPass(frame.cmd);
}
