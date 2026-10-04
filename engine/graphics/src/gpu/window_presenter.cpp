#include "engine/gpu/window_presenter.hpp"

#include "gpu/vk_check.hpp"

bool WindowPresenter::init(GpuDevice* gpu, SDL_Window* window, const VkFormat swapchain_format) {
    this->gpu = gpu;
    this->window = window;
    this->frame_index = 0;
    this->resize_pending = false;

    return this->swapchain.init(gpu, window, swapchain_format)
        && this->targets.init(gpu, &this->swapchain)
        && this->scheduler.init(gpu);
}

void WindowPresenter::shutdown() {
    if (this->gpu == nullptr) {
        return;
    }
    this->gpu->wait_idle();
    this->scheduler.shutdown();
    this->targets.shutdown();
    this->swapchain.shutdown();
    this->gpu = nullptr;
    this->window = nullptr;
}

void WindowPresenter::mark_resized() {
    this->resize_pending = true;
}

bool WindowPresenter::begin(FrameContext& frame) {
    if (this->resize_pending) {
        this->resize_pending = false;
        if (!this->recreate_swapchain()) {
            // Still minimized: try again next frame.
            this->resize_pending = true;
            return false;
        }
    }

    const FrameScheduler::Slot& slot = this->scheduler.begin();

    VkResult acquired = this->swapchain.acquire(slot.image_available, &this->image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        this->resize_pending = true;
        return false;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        vk_check(acquired, "vkAcquireNextImageKHR");
        return false;
    }

    frame.cmd = slot.cmd;
    frame.target = this->targets.target(this->image_index);
    frame.slot = this->scheduler.current;
    frame.frame_index = this->frame_index;
    this->frame_index += 1;
    return true;
}

void WindowPresenter::end() {
    const FrameScheduler::Slot& slot = this->scheduler.slots[this->scheduler.current];
    VkSemaphore render_finished = this->swapchain.render_finished[this->image_index];
    if (!this->scheduler.submit(slot.image_available, render_finished)) {
        return;
    }

    VkResult presented = this->swapchain.present(this->image_index);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        this->resize_pending = true;
    } else {
        vk_check(presented, "vkQueuePresentKHR");
    }
}

bool WindowPresenter::recreate_swapchain() {
    this->gpu->wait_idle();
    if (!this->swapchain.recreate()) {
        return false;
    }
    return this->targets.recreate();
}
