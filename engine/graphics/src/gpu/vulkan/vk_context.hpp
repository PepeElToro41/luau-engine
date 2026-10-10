#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/templates/hash_map.hpp"
#include "engine/templates/sparse_list.hpp"
#include "gpu/backend.hpp"

#include <SDL3/SDL.h>
#include <volk.h>

// The Vulkan backend's state, private to src/gpu/vulkan/. One VulkanContext
// per process, reached from GpuContext::impl. Every GPU object the frontend
// holds a handle to lives in one of the SparseList pools below; the handle's
// SparseId is the pool id. Slot 0 of every pool is reserved at init so that
// GPU_NULL_ID never names a live object.
//
// Files:
//   vk_backend.cpp      the GpuBackend table, init / shutdown orchestration
//   vk_device.cpp       instance, surface, physical device, device, queues
//   vk_swapchain.cpp    swapchain + its images registered as textures
//   vk_frame.cpp        frame slots, begin / end frame, deferred releases
//   vk_resources.cpp    buffers, textures, uploads
//   vk_binding.cpp      samplers, bind layouts, descriptor pools, bind groups, uniform ring
//   vk_pipeline.cpp     pipelines and the template render passes they are built against
//   vk_render_pass.cpp  render pass + framebuffer caches, begin / end render pass
//   vk_commands.cpp     the other cmd_* functions
//   vk_formats.cpp      GpuFormat <-> VkFormat, resource states, aspects
//   vk_access.cpp       the public escape hatch (engine/gpu/vulkan/vk_access.hpp)

static constexpr u32 VK_MAX_SWAPCHAIN_IMAGES = 8;
static constexpr u32 VK_UNIFORM_RING_SIZE = 4 * 1024 * 1024;
static constexpr u32 VK_MAX_DESCRIPTOR_POOL_SIZES = 8;

// --- Pool entries ----------------------------------------------------------------

struct VkBufferEntry {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    u64 size = 0;
    u32 usage = 0;
    GpuMemoryKind memory_kind = GPU_MEMORY_DEVICE_LOCAL;
};

struct VkTextureEntry {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    // The view bind groups sample through: `view`, or a reinterpreting one
    // when the texture has a sampled_format.
    VkImageView sampled_view = VK_NULL_HANDLE;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    GpuFormat format = GPU_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 mip_levels = 0;
    u32 usage = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // Last state a cmd_barrier moved it to (or upload_texture left it in).
    // Only the backend's own transitions (end_frame, uploads) read it.
    GpuResourceState state = GPU_STATE_UNDEFINED;
    // Swapchain images: the image belongs to the swapchain, only the view is ours.
    bool external = false;
};

struct VkSamplerEntry {
    VkSampler sampler = VK_NULL_HANDLE;
};

struct VkPipelineEntry {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    u32 push_constant_size = 0;
};

struct VkBindLayoutEntry {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    // Kept so bind group writes know each slot's descriptor type.
    GpuBindLayoutDesc desc;
};

struct VkBindGroupEntry {
    VkDescriptorSet set = VK_NULL_HANDLE;
    // Pool index in the allocator that owns the set.
    u32 pool = 0;
    bool transient = false;
};

struct VkCommandListEntry {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    // The layout of the pipeline currently bound, for push constants and sets.
    VkPipelineLayout bound_layout = VK_NULL_HANDLE;
    u32 bound_push_size = 0;
};

// --- Descriptor pools ------------------------------------------------------------

// A growing list of descriptor pools of one configuration. Transient
// allocators are reset whole once per frame slot; the long-lived one has
// the FREE flag and frees sets one by one.
struct VkDescriptorPools {
    DynamicArray<VkDescriptorPool> pools;
    VkDescriptorPoolSize sizes[VK_MAX_DESCRIPTOR_POOL_SIZES] = {};
    u32 size_count = 0;
    u32 max_sets = 0;
    VkDescriptorPoolCreateFlags flags = 0;
    u32 current = 0;
};

// --- Deferred destruction ----------------------------------------------------------

// Vulkan objects queued for destruction once the frame slot that could still
// use them has completed. Any subset of the fields may be set.
struct VkPending {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView sampled_view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool set_pool = VK_NULL_HANDLE;
};

// --- Frames ----------------------------------------------------------------------

struct VkFrameSlot {
    VkFence in_flight = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    SparseId command_list = GPU_NULL_ID;
    VkDescriptorPools transient_sets;
    DynamicArray<SparseId> transient_groups;
    GpuBuffer uniform_ring;
    u64 uniform_cursor = 0;
    DynamicArray<VkPending> releases;
};

// --- Caches ----------------------------------------------------------------------

struct VkRenderPassEntry {
    VkRenderPass render_pass = VK_NULL_HANDLE;
    u32 attachment_count = 0;
};

struct VkFramebufferEntry {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    // The textures it views, so it can be dropped when one of them goes.
    SparseId textures[GPU_MAX_COLOR_ATTACHMENTS + 1] = {};
    u32 texture_count = 0;
    u32 width = 0;
    u32 height = 0;
};

// --- The context -------------------------------------------------------------------

struct VulkanContext {
    GpuContext* gpu = nullptr;
    SDL_Window* window = nullptr;
    bool validation = false;

    // Device (vk_device.cpp)
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    VkDevice device = VK_NULL_HANDLE;
    u32 graphics_queue_family = UINT32_MAX;
    u32 present_queue_family = UINT32_MAX;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    bool has_debug_utils = false;

    // Swapchain (vk_swapchain.cpp)
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    VkFormat preferred_swapchain_format = VK_FORMAT_B8G8R8A8_SRGB;
    VkColorSpaceKHR swapchain_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D swapchain_extent = {0, 0};
    u32 swapchain_min_image_count = 0;
    u32 swapchain_image_count = 0;
    VkImage swapchain_images[VK_MAX_SWAPCHAIN_IMAGES] = {};
    SparseId swapchain_textures[VK_MAX_SWAPCHAIN_IMAGES] = {};
    VkSemaphore render_finished[VK_MAX_SWAPCHAIN_IMAGES] = {};
    bool resize_pending = false;

    // Frames (vk_frame.cpp)
    VkFrameSlot slots[FRAMES_IN_FLIGHT];
    u32 current_slot = 0;
    u64 frame_index = 0;
    u32 image_index = 0;
    bool frame_open = false;

    // Uploads (vk_resources.cpp)
    VkCommandPool upload_pool = VK_NULL_HANDLE;
    VkCommandBuffer upload_cmd = VK_NULL_HANDLE;
    VkFence upload_fence = VK_NULL_HANDLE;

    // Pools
    SparseList<VkBufferEntry> buffers;
    SparseList<VkTextureEntry> textures;
    SparseList<VkSamplerEntry> samplers;
    SparseList<VkPipelineEntry> pipelines;
    SparseList<VkBindLayoutEntry> bind_layouts;
    SparseList<VkBindGroupEntry> bind_groups;
    SparseList<VkCommandListEntry> command_lists;

    // Caches (vk_binding.cpp, vk_pipeline.cpp, vk_render_pass.cpp)
    HashMap<u64, SparseId> sampler_cache;
    HashMap<u64, SparseId> bind_layout_cache;
    VkDescriptorPools persistent_sets;
    HashMap<u64, VkRenderPass> template_render_passes; // by GpuTargetFormats hash
    HashMap<u64, VkRenderPassEntry> render_passes;      // by formats + ops
    HashMap<u64, VkFramebufferEntry> framebuffers;      // by render pass + views + extent
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
};

// --- Internal API, one namespace per file ----------------------------------------------

namespace VK_CONTEXT {
VulkanContext& of(GpuContext* gpu);
// Pool lookups: nullptr for a dead or null id.
VkBufferEntry* buffer(VulkanContext& vk, SparseId id);
VkTextureEntry* texture(VulkanContext& vk, SparseId id);
VkSamplerEntry* sampler(VulkanContext& vk, SparseId id);
VkPipelineEntry* pipeline(VulkanContext& vk, SparseId id);
VkBindLayoutEntry* bind_layout(VulkanContext& vk, SparseId id);
VkBindGroupEntry* bind_group(VulkanContext& vk, SparseId id);
VkCommandListEntry* command_list(VulkanContext& vk, SparseId id);
// The command list's context. cmd_* functions resolve their list through this.
VulkanContext& of_command_list(GpuCommandList cmd, VkCommandListEntry** out);
} // namespace VK_CONTEXT

namespace VK_DEVICE {
bool init(VulkanContext& vk);
void shutdown(VulkanContext& vk);
u32 find_memory_type(const VulkanContext& vk, u32 type_bits, VkMemoryPropertyFlags properties);
bool allocate_memory(VulkanContext& vk, const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties, VkDeviceMemory& out, const char* what);
} // namespace VK_DEVICE

namespace VK_SWAPCHAIN {
bool init(VulkanContext& vk);
void shutdown(VulkanContext& vk);
// False while the window is minimized (zero-sized surface).
bool recreate(VulkanContext& vk);
VkResult acquire(VulkanContext& vk, VkSemaphore image_available, u32* image_index);
VkResult present(VulkanContext& vk, u32 image_index);
} // namespace VK_SWAPCHAIN

namespace VK_FRAME {
bool init(VulkanContext& vk);
void shutdown(VulkanContext& vk);
// Queues `pending` on the current slot.
void release(VulkanContext& vk, const VkPending& pending);
// Destroys `pending` now.
void destroy(VulkanContext& vk, const VkPending& pending);
// Destroys everything queued on every slot. The GPU must be idle.
void flush(VulkanContext& vk);
} // namespace VK_FRAME

namespace VK_RESOURCES {
bool init(VulkanContext& vk);
void shutdown(VulkanContext& vk);
// Registers a swapchain image as an external texture; returns its id.
SparseId register_external_texture(VulkanContext& vk, VkImage image, VkImageView view, VkFormat format, u32 width, u32 height);
// Destroys the view of an external texture and drops the pool entry.
void unregister_external_texture(VulkanContext& vk, SparseId id);
// Drops every framebuffer viewing `texture` (deferred).
void forget_texture_framebuffers(VulkanContext& vk, SparseId texture);
} // namespace VK_RESOURCES

namespace VK_BINDING {
bool init(VulkanContext& vk);
void shutdown(VulkanContext& vk);
bool pools_init(VulkanContext& vk, VkDescriptorPools& pools, const VkDescriptorPoolSize* sizes, u32 size_count, u32 max_sets, VkDescriptorPoolCreateFlags flags);
void pools_shutdown(VulkanContext& vk, VkDescriptorPools& pools);
VkDescriptorSet pools_allocate(VulkanContext& vk, VkDescriptorPools& pools, VkDescriptorSetLayout layout, u32* pool_index);
void pools_reset(VulkanContext& vk, VkDescriptorPools& pools);
// Called at the top of a slot's frame: resets its transient pools and
// drops the transient group ids, rewinds its uniform ring.
void begin_slot(VulkanContext& vk, VkFrameSlot& slot);
} // namespace VK_BINDING

namespace VK_PIPELINE {
void shutdown(VulkanContext& vk);
// The render pass pipelines with these target formats are built against
// (cached). Any render pass with the same formats is compatible with it.
VkRenderPass template_render_pass(VulkanContext& vk, const GpuTargetFormats& formats);
} // namespace VK_PIPELINE

namespace VK_RENDER_PASS {
void shutdown(VulkanContext& vk);
// The two external dependencies every render pass carries. Compatibility
// requires identical dependencies, so the template passes pipelines are
// built against use the same ones.
void standard_dependencies(VkSubpassDependency out[2]);
} // namespace VK_RENDER_PASS

namespace VK_COMMANDS {
// Records the barrier moving `texture` from `from` to `to` and updates its
// tracked state. Always emits, even for from == to: that is how writes of
// a previous use of the same image become visible to the next.
void barrier(VkCommandBuffer cmd, VkTextureEntry& texture, GpuResourceState from, GpuResourceState to);
} // namespace VK_COMMANDS

namespace VK_FORMATS {
VkFormat to_vk(GpuFormat format);
GpuFormat from_vk(VkFormat format);
VkImageAspectFlags aspect(GpuFormat format);

struct StateInfo {
    VkImageLayout layout;
    VkPipelineStageFlags stage;
    VkAccessFlags access;
};
// `as_source`: the state is the one being left (its stage mask is what to
// wait for) rather than entered.
StateInfo state_info(GpuResourceState state, bool as_source);
VkAttachmentLoadOp to_vk(GpuLoadOp op);
VkAttachmentStoreOp to_vk(GpuStoreOp op);
VkDescriptorType to_vk(GpuBindingType type);
VkShaderStageFlags to_vk_stages(u8 stages);
VkBufferUsageFlags buffer_usage(u32 usage);
VkImageUsageFlags texture_usage(u32 usage);
// The best supported depth attachment format of the device.
VkFormat find_depth_format(const VulkanContext& vk);
} // namespace VK_FORMATS
