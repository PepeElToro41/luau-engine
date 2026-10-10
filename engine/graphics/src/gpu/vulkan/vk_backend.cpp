#include "gpu/vulkan/vk_check.hpp"
#include "gpu/vulkan/vk_context.hpp"

// The one Vulkan context of the process (see gpu.hpp: one GpuContext, one
// backend, chosen at init).
static VulkanContext g_vk;

// --- VK_CONTEXT ------------------------------------------------------------------

VulkanContext& VK_CONTEXT::of(GpuContext* gpu) {
    return *static_cast<VulkanContext*>(gpu->impl);
}

VkBufferEntry* VK_CONTEXT::buffer(VulkanContext& vk, const SparseId id) {
    return id != GPU_NULL_ID ? vk.buffers.get_element_alive(id) : nullptr;
}
VkTextureEntry* VK_CONTEXT::texture(VulkanContext& vk, const SparseId id) {
    return id != GPU_NULL_ID ? vk.textures.get_element_alive(id) : nullptr;
}
VkSamplerEntry* VK_CONTEXT::sampler(VulkanContext& vk, const SparseId id) {
    return id != GPU_NULL_ID ? vk.samplers.get_element_alive(id) : nullptr;
}
VkPipelineEntry* VK_CONTEXT::pipeline(VulkanContext& vk, const SparseId id) {
    return id != GPU_NULL_ID ? vk.pipelines.get_element_alive(id) : nullptr;
}
VkBindLayoutEntry* VK_CONTEXT::bind_layout(VulkanContext& vk, const SparseId id) {
    return id != GPU_NULL_ID ? vk.bind_layouts.get_element_alive(id) : nullptr;
}
VkBindGroupEntry* VK_CONTEXT::bind_group(VulkanContext& vk, const SparseId id) {
    return id != GPU_NULL_ID ? vk.bind_groups.get_element_alive(id) : nullptr;
}
VkCommandListEntry* VK_CONTEXT::command_list(VulkanContext& vk, const SparseId id) {
    return id != GPU_NULL_ID ? vk.command_lists.get_element_alive(id) : nullptr;
}

VulkanContext& VK_CONTEXT::of_command_list(const GpuCommandList cmd, VkCommandListEntry** out) {
    *out = command_list(g_vk, cmd.id);
    return g_vk;
}

// --- Init / shutdown ----------------------------------------------------------------

static bool vk_init(GpuContext* gpu, const GpuInitDesc& desc) {
    VulkanContext& vk = g_vk;
    if (vk.gpu != nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] init: already initialized");
        return false;
    }
    if (desc.window == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] init: a window is required");
        return false;
    }
    vk.gpu = gpu;
    gpu->impl = &vk;
    vk.window = desc.window;
    vk.validation = desc.validation;
    vk.preferred_swapchain_format = VK_FORMATS::to_vk(desc.swapchain_format);
    if (vk.preferred_swapchain_format == VK_FORMAT_UNDEFINED) {
        vk.preferred_swapchain_format = VK_FORMAT_B8G8R8A8_SRGB;
    }

    // Slot 0 of every pool is GPU_NULL_ID.
    vk.buffers.new_element();
    vk.textures.new_element();
    vk.samplers.new_element();
    vk.pipelines.new_element();
    vk.bind_layouts.new_element();
    vk.bind_groups.new_element();
    vk.command_lists.new_element();

    return VK_DEVICE::init(vk) && VK_RESOURCES::init(vk) && VK_BINDING::init(vk) && VK_SWAPCHAIN::init(vk) && VK_FRAME::init(vk);
}

static void vk_shutdown(GpuContext* gpu) {
    VulkanContext& vk = g_vk;
    if (vk.gpu == nullptr) {
        return;
    }
    (void)gpu;
    if (vk.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk.device);
        VK_FRAME::shutdown(vk);
        VK_RENDER_PASS::shutdown(vk);
        VK_PIPELINE::shutdown(vk);
        VK_SWAPCHAIN::shutdown(vk);
        VK_BINDING::shutdown(vk);
        VK_RESOURCES::shutdown(vk);
    }
    VK_DEVICE::shutdown(vk);
    vk.buffers.free();
    vk.textures.free();
    vk.samplers.free();
    vk.pipelines.free();
    vk.bind_layouts.free();
    vk.bind_groups.free();
    vk.command_lists.free();
    vk.sampler_cache.free();
    vk.bind_layout_cache.free();
    vk.template_render_passes.free();
    vk.render_passes.free();
    vk.framebuffers.free();
    for (VkFrameSlot& slot : vk.slots) {
        slot.releases.free();
        slot.transient_groups.free();
        slot.transient_sets.pools.free();
    }
    vk.persistent_sets.pools.free();
    vk.gpu->impl = nullptr;
    vk.gpu = nullptr;
    vk.window = nullptr;
    vk = VulkanContext{};
}

// --- The table -----------------------------------------------------------------------

namespace VK_FRAME_TABLE {
void fill(GpuBackend& table);
}
namespace VK_RESOURCES_TABLE {
void fill(GpuBackend& table);
}
namespace VK_BINDING_TABLE {
void fill(GpuBackend& table);
}
namespace VK_PIPELINE_TABLE {
void fill(GpuBackend& table);
}
namespace VK_RENDER_PASS_TABLE {
void fill(GpuBackend& table);
}
namespace VK_COMMANDS_TABLE {
void fill(GpuBackend& table);
}

const GpuBackend* VULKAN_BACKEND::table() {
    static GpuBackend backend = {};
    static bool filled = false;
    if (!filled) {
        backend.init = vk_init;
        backend.shutdown = vk_shutdown;
        VK_FRAME_TABLE::fill(backend);
        VK_RESOURCES_TABLE::fill(backend);
        VK_BINDING_TABLE::fill(backend);
        VK_PIPELINE_TABLE::fill(backend);
        VK_RENDER_PASS_TABLE::fill(backend);
        VK_COMMANDS_TABLE::fill(backend);
        filled = true;
    }
    return &backend;
}
