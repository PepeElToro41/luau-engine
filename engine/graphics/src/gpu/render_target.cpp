#include "engine/gpu/render_target.hpp"

#include "gpu/vk_check.hpp"

#include <cstdio>

// --- Render pass ---------------------------------------------------------------

VkRenderPass create_color_render_pass(const GpuDevice* gpu, const VkFormat format, const VkImageLayout final_layout) {
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = final_layout;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dependencies[2] = {};
    u32 dependency_count = 1;

    // Before: wait for whoever last touched the image. For a present target
    // that is the acquire semaphore (waited at color-attachment-output); for a
    // sampled target it is the fragment shader that read it last frame.
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (final_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;

        // After: make the writes visible to the fragment shader that samples
        // the result later in the same command buffer.
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependency_count = 2;
    } else {
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = 0;
    }

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = dependency_count;
    info.pDependencies = dependencies;

    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (!vk_check(vkCreateRenderPass(gpu->device, &info, nullptr, &render_pass), "vkCreateRenderPass")) {
        return VK_NULL_HANDLE;
    }
    return render_pass;
}

static VkFramebuffer create_framebuffer(const GpuDevice* gpu, VkRenderPass render_pass, VkImageView view, VkExtent2D extent) {
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = render_pass;
    info.attachmentCount = 1;
    info.pAttachments = &view;
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (!vk_check(vkCreateFramebuffer(gpu->device, &info, nullptr, &framebuffer), "vkCreateFramebuffer")) {
        return VK_NULL_HANDLE;
    }
    return framebuffer;
}

// --- SwapchainTargets ----------------------------------------------------------

bool SwapchainTargets::init(GpuDevice* gpu, const Swapchain* swapchain) {
    this->gpu = gpu;
    this->swapchain = swapchain;
    return this->recreate();
}

void SwapchainTargets::shutdown() {
    if (this->gpu == nullptr) {
        return;
    }
    this->destroy_framebuffers();
    if (this->render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(this->gpu->device, this->render_pass, nullptr);
        this->render_pass = VK_NULL_HANDLE;
    }
    this->format = VK_FORMAT_UNDEFINED;
    this->gpu = nullptr;
    this->swapchain = nullptr;
}

bool SwapchainTargets::recreate() {
    this->destroy_framebuffers();

    if (this->render_pass != VK_NULL_HANDLE && this->format != this->swapchain->format) {
        vkDestroyRenderPass(this->gpu->device, this->render_pass, nullptr);
        this->render_pass = VK_NULL_HANDLE;
    }
    if (this->render_pass == VK_NULL_HANDLE) {
        this->render_pass = create_color_render_pass(this->gpu, this->swapchain->format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        if (this->render_pass == VK_NULL_HANDLE) {
            return false;
        }
        this->format = this->swapchain->format;
    }

    for (u32 i = 0; i < this->swapchain->image_count; ++i) {
        this->framebuffers[i] = create_framebuffer(this->gpu, this->render_pass, this->swapchain->views[i], this->swapchain->extent);
        if (this->framebuffers[i] == VK_NULL_HANDLE) {
            return false;
        }
    }
    return true;
}

RenderTarget SwapchainTargets::target(const u32 image_index) const {
    RenderTarget target;
    target.render_pass = this->render_pass;
    target.framebuffer = this->framebuffers[image_index];
    target.view = this->swapchain->views[image_index];
    target.format = this->format;
    target.extent = this->swapchain->extent;
    return target;
}

void SwapchainTargets::destroy_framebuffers() {
    for (u32 i = 0; i < MAX_SWAPCHAIN_IMAGES; ++i) {
        if (this->framebuffers[i] != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(this->gpu->device, this->framebuffers[i], nullptr);
            this->framebuffers[i] = VK_NULL_HANDLE;
        }
    }
}

// --- OffscreenTarget -----------------------------------------------------------

bool OffscreenTarget::init(GpuDevice* gpu, const VkFormat format, const VkExtent2D extent, const VkFormat sampled_format) {
    this->gpu = gpu;
    this->format = format;
    this->sampled_format = sampled_format == VK_FORMAT_UNDEFINED ? format : sampled_format;

    this->render_pass = create_color_render_pass(gpu, format, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (this->render_pass == VK_NULL_HANDLE) {
        return false;
    }

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 1.0f;
    if (!vk_check(vkCreateSampler(gpu->device, &sampler_info, nullptr, &this->sampler), "vkCreateSampler")) {
        this->sampler = VK_NULL_HANDLE;
        return false;
    }

    return this->create_image(extent);
}

void OffscreenTarget::shutdown() {
    if (this->gpu == nullptr) {
        return;
    }
    this->destroy_image();
    if (this->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(this->gpu->device, this->sampler, nullptr);
        this->sampler = VK_NULL_HANDLE;
    }
    if (this->render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(this->gpu->device, this->render_pass, nullptr);
        this->render_pass = VK_NULL_HANDLE;
    }
    this->gpu = nullptr;
}

bool OffscreenTarget::resize(const VkExtent2D extent) {
    this->destroy_image();
    return this->create_image(extent);
}

RenderTarget OffscreenTarget::target() const {
    RenderTarget target;
    target.render_pass = this->render_pass;
    target.framebuffer = this->framebuffer;
    target.view = this->view;
    target.format = this->format;
    target.extent = this->extent;
    return target;
}

bool OffscreenTarget::create_image(const VkExtent2D extent) {
    if (extent.width == 0 || extent.height == 0) {
        fprintf(stderr, "[vulkan] offscreen target needs a non-zero extent\n");
        return false;
    }
    VkDevice device = this->gpu->device;

    const bool mutable_format = this->sampled_format != this->format;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.flags = mutable_format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = this->format;
    image_info.extent = {extent.width, extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vk_check(vkCreateImage(device, &image_info, nullptr, &this->image), "vkCreateImage (offscreen)")) {
        this->image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, this->image, &requirements);
    const u32 memory_type = this->gpu->find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        fprintf(stderr, "[vulkan] no device-local memory type for the offscreen target\n");
        return false;
    }

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    if (!vk_check(vkAllocateMemory(device, &alloc_info, nullptr, &this->memory), "vkAllocateMemory (offscreen)")) {
        this->memory = VK_NULL_HANDLE;
        return false;
    }
    if (!vk_check(vkBindImageMemory(device, this->image, this->memory, 0), "vkBindImageMemory (offscreen)")) {
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = this->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = this->format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (!vk_check(vkCreateImageView(device, &view_info, nullptr, &this->view), "vkCreateImageView (offscreen)")) {
        this->view = VK_NULL_HANDLE;
        return false;
    }

    if (mutable_format) {
        view_info.format = this->sampled_format;
        if (!vk_check(vkCreateImageView(device, &view_info, nullptr, &this->sampled_view), "vkCreateImageView (offscreen sampled)")) {
            this->sampled_view = VK_NULL_HANDLE;
            return false;
        }
    } else {
        this->sampled_view = this->view;
    }

    this->framebuffer = create_framebuffer(this->gpu, this->render_pass, this->view, extent);
    if (this->framebuffer == VK_NULL_HANDLE) {
        return false;
    }

    this->extent = extent;
    return true;
}

void OffscreenTarget::destroy_image() {
    VkDevice device = this->gpu->device;
    if (this->framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, this->framebuffer, nullptr);
        this->framebuffer = VK_NULL_HANDLE;
    }
    if (this->sampled_view != VK_NULL_HANDLE && this->sampled_view != this->view) {
        vkDestroyImageView(device, this->sampled_view, nullptr);
    }
    this->sampled_view = VK_NULL_HANDLE;
    if (this->view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, this->view, nullptr);
        this->view = VK_NULL_HANDLE;
    }
    if (this->image != VK_NULL_HANDLE) {
        vkDestroyImage(device, this->image, nullptr);
        this->image = VK_NULL_HANDLE;
    }
    if (this->memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, this->memory, nullptr);
        this->memory = VK_NULL_HANDLE;
    }
    this->extent = {0, 0};
}
