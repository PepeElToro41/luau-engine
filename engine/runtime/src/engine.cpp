#include "engine/engine.h"

bool Engine::init(GpuDevice* gpu) {
    this->gpu = gpu;
    this->time = 0.0;
    if (this->create_singleton<AssetResourceProvider>() == nullptr) {
        return false;
    }
    return true;
}

void Engine::shutdown() {
    // Singletons that own memory release it before the store destroys them.
    if (AssetResourceProvider* assets = this->get_singleton<AssetResourceProvider>()) {
        assets->free();
    }
    this->singletons.free();
    this->gpu = nullptr;
}

void Engine::update(const f32 dt) {
    this->time += dt;
}

void Engine::render(const FrameContext& frame) {
    VkClearValue clear{};
    clear.color.float32[0] = this->clear_color[0];
    clear.color.float32[1] = this->clear_color[1];
    clear.color.float32[2] = this->clear_color[2];
    clear.color.float32[3] = this->clear_color[3];

    VkRenderPassBeginInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_info.renderPass = frame.target.render_pass;
    pass_info.framebuffer = frame.target.framebuffer;
    pass_info.renderArea.extent = frame.target.extent;
    pass_info.clearValueCount = 1;
    pass_info.pClearValues = &clear;

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
