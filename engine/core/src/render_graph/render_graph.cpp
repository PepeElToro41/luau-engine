#include "engine/render_graph/render_graph.hpp"

#include "engine/memory/heap_allocator.hpp"
#include "engine/utils/hash.hpp"

#include <cstring>

// --- Names ---------------------------------------------------------------------

// Copies `name` into a NAME_MAX buffer. False if empty or too long.
static bool copy_name(char* out, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    const usz length = strlen(name);
    if (length >= RENDER_GRAPH::NAME_MAX) {
        return false;
    }
    memcpy(out, name, length + 1);
    return true;
}

// --- Lifetime ------------------------------------------------------------------

void RenderGraph::init(BaseAllocator* allocator) {
    if (allocator == nullptr) {
        allocator = MEMORY::heap_allocator();
    }
    this->free();
    this->allocator = allocator;
    this->passes = SparseList<RenderPassDesc>(allocator);
    this->resources = SparseList<RenderResourceDesc>(allocator);
    this->order = DynamicArray<RenderPassHandle>(allocator);
    this->pass_names = HashMap<u64, RenderPassHandle>(allocator);
    this->resource_names = HashMap<u64, RenderResourceHandle>(allocator);

    RenderResourceDesc color;
    color.aspect = RENDER_ASPECT_COLOR;
    color.format = RENDER_FORMAT_BACKBUFFER;
    this->backbuffer = this->add_resource_of_kind("backbuffer", color, RENDER_RESOURCE_BACKBUFFER);

    RenderResourceDesc depth;
    depth.aspect = RENDER_ASPECT_DEPTH;
    depth.format = RENDER_FORMAT_BACKBUFFER;
    this->backbuffer_depth = this->add_resource_of_kind("backbuffer_depth", depth, RENDER_RESOURCE_BACKBUFFER_DEPTH);

    this->dirty = true;
    this->version = 0;
}

void RenderGraph::free() {
    this->passes.free();
    this->resources.free();
    this->order.free();
    this->pass_names.free();
    this->resource_names.free();
    this->backbuffer = RenderResourceHandle{};
    this->backbuffer_depth = RenderResourceHandle{};
    this->dirty = true;
}

void RenderGraph::mark_dirty() {
    this->dirty = true;
    this->version += 1;
}

// --- Resources -----------------------------------------------------------------

RenderResourceHandle RenderGraph::add_resource_of_kind(const char* name, const RenderResourceDesc& desc, const RenderResourceKind kind) {
    char copied[RENDER_GRAPH::NAME_MAX];
    if (!copy_name(copied, name) || this->find_resource(name).is_valid()) {
        return RenderResourceHandle{};
    }
    const u64 hash = HASH::fnv1a_str(copied);
    if (this->resource_names.contains(hash)) {
        // Another name with the same hash: refuse rather than alias it.
        return RenderResourceHandle{};
    }

    RenderResourceHandle handle;
    handle.id = this->resources.new_element();
    RenderResourceDesc* stored = this->resources.get_element_alive(handle.id);
    *stored = desc;
    memcpy(stored->name, copied, sizeof(copied));
    stored->kind = kind;
    this->resource_names.insert(hash, handle);
    this->mark_dirty();
    return handle;
}

RenderResourceHandle RenderGraph::add_resource(const char* name, const RenderResourceDesc& desc) {
    return this->add_resource_of_kind(name, desc, RENDER_RESOURCE_TRANSIENT);
}

bool RenderGraph::remove_resource(const RenderResourceHandle handle) {
    RenderResourceDesc* desc = this->resource(handle);
    if (desc == nullptr || desc->kind != RENDER_RESOURCE_TRANSIENT) {
        return false;
    }
    this->resource_names.remove(HASH::fnv1a_str(desc->name));
    this->resources.delete_element(handle.id);
    this->mark_dirty();
    return true;
}

RenderResourceHandle RenderGraph::find_resource(const char* name) const {
    if (name == nullptr) {
        return RenderResourceHandle{};
    }
    const RenderResourceHandle* found = this->resource_names.find(HASH::fnv1a_str(name));
    if (found == nullptr) {
        return RenderResourceHandle{};
    }
    const RenderResourceDesc* desc = this->resource(*found);
    if (desc == nullptr || strcmp(desc->name, name) != 0) {
        return RenderResourceHandle{};
    }
    return *found;
}

RenderResourceDesc* RenderGraph::resource(const RenderResourceHandle handle) {
    if (!handle.is_valid()) {
        return nullptr;
    }
    return this->resources.get_element_alive(handle.id);
}

const RenderResourceDesc* RenderGraph::resource(const RenderResourceHandle handle) const {
    if (!handle.is_valid()) {
        return nullptr;
    }
    return this->resources.get_element_alive(handle.id);
}

// --- Passes --------------------------------------------------------------------

RenderPassHandle RenderGraph::add_pass(const char* name, const RenderPassKind kind) {
    char copied[RENDER_GRAPH::NAME_MAX];
    if (!copy_name(copied, name) || this->find_pass(name).is_valid()) {
        return RenderPassHandle{};
    }
    const u64 hash = HASH::fnv1a_str(copied);
    if (this->pass_names.contains(hash)) {
        return RenderPassHandle{};
    }

    RenderPassHandle handle;
    handle.id = this->passes.new_element();
    RenderPassDesc* stored = this->passes.get_element_alive(handle.id);
    *stored = RenderPassDesc{};
    memcpy(stored->name, copied, sizeof(copied));
    stored->kind = kind;
    this->pass_names.insert(hash, handle);
    this->order.push(handle);
    this->mark_dirty();
    return handle;
}

bool RenderGraph::remove_pass(const RenderPassHandle handle) {
    RenderPassDesc* desc = this->pass(handle);
    if (desc == nullptr) {
        return false;
    }
    this->pass_names.remove(HASH::fnv1a_str(desc->name));
    for (usz i = 0; i < this->order.count; ++i) {
        if (this->order[i] == handle) {
            this->order.remove_at(i);
            break;
        }
    }
    this->passes.delete_element(handle.id);
    this->mark_dirty();
    return true;
}

RenderPassHandle RenderGraph::find_pass(const char* name) const {
    if (name == nullptr) {
        return RenderPassHandle{};
    }
    const RenderPassHandle* found = this->pass_names.find(HASH::fnv1a_str(name));
    if (found == nullptr) {
        return RenderPassHandle{};
    }
    const RenderPassDesc* desc = this->pass(*found);
    if (desc == nullptr || strcmp(desc->name, name) != 0) {
        return RenderPassHandle{};
    }
    return *found;
}

RenderPassDesc* RenderGraph::pass(const RenderPassHandle handle) {
    if (!handle.is_valid()) {
        return nullptr;
    }
    return this->passes.get_element_alive(handle.id);
}

const RenderPassDesc* RenderGraph::pass(const RenderPassHandle handle) const {
    if (!handle.is_valid()) {
        return nullptr;
    }
    return this->passes.get_element_alive(handle.id);
}

bool RenderGraph::move_pass(const RenderPassHandle handle, const RenderPassHandle before) {
    if (this->pass(handle) == nullptr) {
        return false;
    }
    if (before.is_valid() && (this->pass(before) == nullptr || before == handle)) {
        return false;
    }
    for (usz i = 0; i < this->order.count; ++i) {
        if (this->order[i] == handle) {
            this->order.remove_at(i);
            break;
        }
    }
    usz position = this->order.count;
    if (before.is_valid()) {
        for (usz i = 0; i < this->order.count; ++i) {
            if (this->order[i] == before) {
                position = i;
                break;
            }
        }
    }
    this->order.push(RenderPassHandle{});
    for (usz i = this->order.count - 1; i > position; --i) {
        this->order[i] = this->order[i - 1];
    }
    this->order[position] = handle;
    this->mark_dirty();
    return true;
}

// --- Structural edits ----------------------------------------------------------

bool RenderGraph::set_color_attachment(const RenderPassHandle pass, const u32 index, const RenderResourceHandle resource, const RenderLoadOp load, const RenderStoreOp store) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || index >= RENDER_GRAPH::MAX_COLOR_ATTACHMENTS || index > desc->color_count || this->resource(resource) == nullptr) {
        return false;
    }
    RenderAttachment& attachment = desc->color[index];
    if (index == desc->color_count) {
        attachment = RenderAttachment{};
        desc->color_count += 1;
    }
    attachment.resource = resource;
    attachment.load = load;
    attachment.store = store;
    this->mark_dirty();
    return true;
}

bool RenderGraph::remove_color_attachment(const RenderPassHandle pass, const u32 index) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || index >= desc->color_count) {
        return false;
    }
    for (u32 i = index + 1; i < desc->color_count; ++i) {
        desc->color[i - 1] = desc->color[i];
    }
    desc->color_count -= 1;
    desc->color[desc->color_count] = RenderAttachment{};
    this->mark_dirty();
    return true;
}

bool RenderGraph::set_depth_attachment(const RenderPassHandle pass, const RenderResourceHandle resource, const RenderLoadOp load, const RenderStoreOp store) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || this->resource(resource) == nullptr) {
        return false;
    }
    if (!desc->has_depth) {
        desc->depth = RenderAttachment{};
        desc->depth.clear[0] = 1.0f;
        desc->has_depth = true;
    }
    desc->depth.resource = resource;
    desc->depth.load = load;
    desc->depth.store = store;
    this->mark_dirty();
    return true;
}

bool RenderGraph::clear_depth_attachment(const RenderPassHandle pass) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || !desc->has_depth) {
        return false;
    }
    desc->depth = RenderAttachment{};
    desc->has_depth = false;
    this->mark_dirty();
    return true;
}

bool RenderGraph::add_input(const RenderPassHandle pass, const RenderResourceHandle resource) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || this->resource(resource) == nullptr || desc->input_count >= RENDER_GRAPH::MAX_INPUTS) {
        return false;
    }
    for (u32 i = 0; i < desc->input_count; ++i) {
        if (desc->inputs[i] == resource) {
            return false;
        }
    }
    desc->inputs[desc->input_count++] = resource;
    this->mark_dirty();
    return true;
}

bool RenderGraph::remove_input(const RenderPassHandle pass, const RenderResourceHandle resource) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr) {
        return false;
    }
    for (u32 i = 0; i < desc->input_count; ++i) {
        if (desc->inputs[i] == resource) {
            for (u32 j = i + 1; j < desc->input_count; ++j) {
                desc->inputs[j - 1] = desc->inputs[j];
            }
            desc->input_count -= 1;
            desc->inputs[desc->input_count] = RenderResourceHandle{};
            this->mark_dirty();
            return true;
        }
    }
    return false;
}

bool RenderGraph::set_enabled(const RenderPassHandle pass, const bool enabled) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr) {
        return false;
    }
    if (desc->enabled != enabled) {
        desc->enabled = enabled;
        this->mark_dirty();
    }
    return true;
}

bool RenderGraph::set_clear_target(const RenderPassHandle pass, const RenderResourceHandle resource) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || this->resource(resource) == nullptr) {
        return false;
    }
    desc->clear_target = resource;
    this->mark_dirty();
    return true;
}

bool RenderGraph::set_blit(const RenderPassHandle pass, const RenderResourceHandle src, const RenderResourceHandle dst, const bool linear) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || this->resource(src) == nullptr || this->resource(dst) == nullptr) {
        return false;
    }
    desc->blit_src = src;
    desc->blit_dst = dst;
    desc->blit_linear = linear;
    this->mark_dirty();
    return true;
}

// --- Live edits ----------------------------------------------------------------

bool RenderGraph::set_clear_color(const RenderPassHandle pass, const u32 index, const f32 rgba[4]) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || rgba == nullptr) {
        return false;
    }
    if (desc->kind == RENDER_PASS_CLEAR) {
        memcpy(desc->clear_color, rgba, sizeof(desc->clear_color));
        return true;
    }
    if (index >= desc->color_count) {
        return false;
    }
    memcpy(desc->color[index].clear, rgba, sizeof(desc->color[index].clear));
    return true;
}

bool RenderGraph::set_clear_depth(const RenderPassHandle pass, const f32 depth, const u32 stencil) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr) {
        return false;
    }
    if (desc->kind == RENDER_PASS_CLEAR) {
        desc->clear_depth = depth;
        desc->clear_stencil = stencil;
        return true;
    }
    if (!desc->has_depth) {
        return false;
    }
    desc->depth.clear[0] = depth;
    desc->depth.clear_stencil = stencil;
    return true;
}

bool RenderGraph::set_constants(const RenderPassHandle pass, const void* data, const u32 size) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || size > RENDER_GRAPH::MAX_CONSTANT_BYTES || (size > 0 && data == nullptr)) {
        return false;
    }
    if (size > 0) {
        memcpy(desc->constants, data, size);
    }
    desc->constant_size = size;
    return true;
}

bool RenderGraph::set_draw_scene_tag(const RenderPassHandle pass, const char* tag) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || !copy_name(desc->tag, tag)) {
        return false;
    }
    desc->tag_hash = HASH::fnv1a_str(desc->tag);
    return true;
}

bool RenderGraph::set_fullscreen_shader(const RenderPassHandle pass, const EntityId shader) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr) {
        return false;
    }
    desc->shader = shader;
    return true;
}

bool RenderGraph::set_custom_callback(const RenderPassHandle pass, const char* name) {
    RenderPassDesc* desc = this->pass(pass);
    if (desc == nullptr || !copy_name(desc->callback, name)) {
        return false;
    }
    desc->callback_hash = HASH::fnv1a_str(desc->callback);
    return true;
}
