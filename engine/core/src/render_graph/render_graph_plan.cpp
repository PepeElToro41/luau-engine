#include "engine/render_graph/render_graph_plan.hpp"

#include "engine/memory/heap_allocator.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/utils/hash.hpp"

#include <cmath>

// --- Names ---------------------------------------------------------------------

const char* RENDER_GRAPH::state_name(const RenderResourceState state) {
    switch (state) {
    case RENDER_STATE_UNDEFINED:
        return "undefined";
    case RENDER_STATE_COLOR_ATTACHMENT:
        return "color_attachment";
    case RENDER_STATE_DEPTH_ATTACHMENT:
        return "depth_attachment";
    case RENDER_STATE_SAMPLED:
        return "sampled";
    case RENDER_STATE_TRANSFER_SRC:
        return "transfer_src";
    case RENDER_STATE_TRANSFER_DST:
        return "transfer_dst";
    case RENDER_STATE_STORAGE:
        return "storage";
    case RENDER_STATE_EXTERNAL:
        return "external";
    }
    return "unknown";
}

const char* RENDER_GRAPH::pass_kind_name(const RenderPassKind kind) {
    switch (kind) {
    case RENDER_PASS_DRAW_SCENE:
        return "draw_scene";
    case RENDER_PASS_FULLSCREEN:
        return "fullscreen";
    case RENDER_PASS_CLEAR:
        return "clear";
    case RENDER_PASS_BLIT:
        return "blit";
    case RENDER_PASS_CUSTOM:
        return "custom";
    case RENDER_PASS_COMPUTE:
        return "compute";
    }
    return "unknown";
}

u64 RENDER_GRAPH::compat_key(const u32 color_count, const RenderFormat* color_formats, const RenderFormat depth_format) {
    u64 key = HASH::fnv1a(&color_count, sizeof(color_count));
    for (u32 i = 0; i < color_count; ++i) {
        key = HASH::fnv1a_append(key, &color_formats[i], sizeof(RenderFormat));
    }
    key = HASH::fnv1a_append(key, &depth_format, sizeof(depth_format));
    const u32 samples = 1;
    key = HASH::fnv1a_append(key, &samples, sizeof(samples));
    return key;
}

u64 RENDER_GRAPH::target_compat_key(const RenderBackbufferInfo& backbuffer) {
    u64 key = compat_key(1, &backbuffer.color_format, backbuffer.depth_format);
    const u32 marker = 0x54475254u; // "TRGT": the app's render pass, not one of ours
    return HASH::fnv1a_append(key, &marker, sizeof(marker));
}

u64 RENDER_GRAPH::pass_compat_key(const PlannedPass& pass) {
    RenderFormat formats[MAX_COLOR_ATTACHMENTS];
    for (u32 i = 0; i < pass.color_count; ++i) {
        formats[i] = pass.color[i].format;
    }
    u64 key = compat_key(pass.color_count, formats, pass.has_depth ? pass.depth.format : RENDER_FORMAT_UNDEFINED);
    for (u32 i = 0; i < pass.color_count; ++i) {
        const u32 states[2] = {pass.color[i].previous, pass.color[i].final};
        key = HASH::fnv1a_append(key, states, sizeof(states));
    }
    if (pass.has_depth) {
        const u32 states[2] = {pass.depth.previous, pass.depth.final};
        key = HASH::fnv1a_append(key, states, sizeof(states));
    }
    return key;
}

// --- RenderGraphPlan -----------------------------------------------------------

void RenderGraphPlan::init(BaseAllocator* allocator) {
    if (allocator == nullptr) {
        allocator = MEMORY::heap_allocator();
    }
    this->free();
    this->resources = DynamicArray<PlannedResource>(allocator);
    this->passes = DynamicArray<PlannedPass>(allocator);
    this->compat_keys = DynamicArray<u64>(allocator);
}

void RenderGraphPlan::free() {
    this->resources.free();
    this->passes.free();
    this->compat_keys.free();
    this->backbuffer = RenderBackbufferInfo{};
    this->backbuffer_pass = RENDER_GRAPH::NO_RESOURCE;
    this->graph_version = 0;
}

void RenderGraphPlan::clear() {
    this->resources.clear();
    this->passes.clear();
    this->compat_keys.clear();
    this->backbuffer = RenderBackbufferInfo{};
    this->backbuffer_pass = RENDER_GRAPH::NO_RESOURCE;
    this->graph_version = 0;
}

u32 RenderGraphPlan::find_resource(const RenderResourceHandle handle) const {
    if (!handle.is_valid()) {
        return RENDER_GRAPH::NO_RESOURCE;
    }
    for (usz i = 0; i < this->resources.count; ++i) {
        if (this->resources[i].handle == handle) {
            return static_cast<u32>(i);
        }
    }
    return RENDER_GRAPH::NO_RESOURCE;
}

u32 RenderGraphPlan::find_pass(const RenderPassHandle handle) const {
    if (!handle.is_valid()) {
        return RENDER_GRAPH::NO_RESOURCE;
    }
    for (usz i = 0; i < this->passes.count; ++i) {
        if (this->passes[i].handle == handle) {
            return static_cast<u32>(i);
        }
    }
    return RENDER_GRAPH::NO_RESOURCE;
}

// --- Compiler ------------------------------------------------------------------

namespace {

struct Compiler {
    const RenderGraph& graph;
    const RenderBackbufferInfo& backbuffer;
    RenderGraphPlan& plan;
    DynamicArray<RenderDiagnostic>* diagnostics;
    bool ok = true;

    void error(const RenderPassHandle pass, const RenderResourceHandle resource, const char* message) {
        this->ok = false;
        this->report(RENDER_DIAG_ERROR, pass, resource, message);
    }
    void warning(const RenderPassHandle pass, const RenderResourceHandle resource, const char* message) {
        this->report(RENDER_DIAG_WARNING, pass, resource, message);
    }
    void report(const RenderDiagnosticSeverity severity, const RenderPassHandle pass, const RenderResourceHandle resource, const char* message) {
        if (this->diagnostics != nullptr) {
            RenderDiagnostic diagnostic;
            diagnostic.severity = severity;
            diagnostic.pass = pass;
            diagnostic.resource = resource;
            diagnostic.message = message;
            this->diagnostics->push(diagnostic);
        }
    }

    bool collect_resources();
    bool collect_passes();
    bool collect_pass(RenderPassHandle handle, const RenderPassDesc& desc, PlannedPass& out);
    bool collect_attachment(const PlannedPass& pass, const RenderPassHandle handle, const RenderAttachment& attachment, RenderResourceAspect aspect, PlannedAttachment& out, u32& width, u32& height);
    void compute_usage();
    void choose_finals();
    void walk(bool record);
    void collect_keys();
};

static u32 scaled(const u32 base, const f32 scale) {
    const f32 value = std::round(static_cast<f32>(base) * scale);
    return value < 1.0f ? 1 : static_cast<u32>(value);
}

static RenderResourceState attachment_state(const RenderResourceAspect aspect) {
    return aspect == RENDER_ASPECT_DEPTH ? RENDER_STATE_DEPTH_ATTACHMENT : RENDER_STATE_COLOR_ATTACHMENT;
}

static bool is_read_state(const RenderResourceState state) {
    return state == RENDER_STATE_SAMPLED || state == RENDER_STATE_TRANSFER_SRC || state == RENDER_STATE_TRANSFER_DST || state == RENDER_STATE_STORAGE;
}

static bool is_backbuffer_kind(const RenderResourceKind kind) {
    return kind != RENDER_RESOURCE_TRANSIENT;
}

bool Compiler::collect_resources() {
    const SparseList<RenderResourceDesc>& resources = this->graph.resources;
    for (usz i = 0; i < resources.alive_count; ++i) {
        const SparseId id = resources.get_alive_id(i);
        const RenderResourceDesc* desc = resources.get_element_any(id);

        PlannedResource resource;
        resource.handle.id = id;
        resource.kind = desc->kind;
        resource.aspect = desc->aspect;
        switch (desc->kind) {
        case RENDER_RESOURCE_BACKBUFFER:
            resource.format = this->backbuffer.color_format;
            resource.width = this->backbuffer.width;
            resource.height = this->backbuffer.height;
            break;
        case RENDER_RESOURCE_BACKBUFFER_DEPTH:
            resource.format = this->backbuffer.depth_format;
            resource.width = this->backbuffer.width;
            resource.height = this->backbuffer.height;
            break;
        case RENDER_RESOURCE_TRANSIENT:
            if (desc->format == RENDER_FORMAT_BACKBUFFER) {
                resource.format = desc->aspect == RENDER_ASPECT_DEPTH ? this->backbuffer.depth_format : this->backbuffer.color_format;
            } else {
                resource.format = desc->format;
            }
            if (desc->size_mode == RENDER_SIZE_RELATIVE) {
                resource.width = scaled(this->backbuffer.width, desc->scale);
                resource.height = scaled(this->backbuffer.height, desc->scale);
            } else {
                resource.width = desc->width;
                resource.height = desc->height;
            }
            break;
        }
        this->plan.resources.push(resource);
    }
    return this->ok;
}

bool Compiler::collect_attachment(const PlannedPass& pass, const RenderPassHandle handle, const RenderAttachment& attachment, const RenderResourceAspect aspect, PlannedAttachment& out, u32& width, u32& height) {
    (void)pass;
    const u32 index = this->plan.find_resource(attachment.resource);
    if (index == RENDER_GRAPH::NO_RESOURCE) {
        this->error(handle, attachment.resource, "attachment references a removed resource");
        return false;
    }
    PlannedResource& resource = this->plan.resources[index];
    if (resource.aspect != aspect) {
        this->error(handle, attachment.resource, aspect == RENDER_ASPECT_DEPTH ? "the depth attachment needs a depth resource" : "a color attachment needs a color resource");
        return false;
    }
    if (resource.format == RENDER_FORMAT_UNDEFINED) {
        this->error(handle, attachment.resource, "resource has no format");
        return false;
    }
    if (resource.width == 0 || resource.height == 0) {
        this->error(handle, attachment.resource, "resource has no size");
        return false;
    }
    if (width == 0 && height == 0) {
        width = resource.width;
        height = resource.height;
    } else if (width != resource.width || height != resource.height) {
        this->error(handle, attachment.resource, "every attachment of a pass must have the same size");
        return false;
    }
    resource.used = true;
    out.resource = index;
    out.format = resource.format;
    out.load = attachment.load;
    out.store = attachment.store;
    out.initial = RENDER_STATE_UNDEFINED;
    out.final = attachment_state(aspect);
    out.previous = RENDER_STATE_UNDEFINED;
    return true;
}

bool Compiler::collect_pass(const RenderPassHandle handle, const RenderPassDesc& desc, PlannedPass& out) {
    out.handle = handle;
    out.kind = desc.kind;
    bool ok = true;

    switch (desc.kind) {
    case RENDER_PASS_COMPUTE:
        this->error(handle, RenderResourceHandle{}, "compute passes are not supported yet");
        return false;
    case RENDER_PASS_CLEAR:
    case RENDER_PASS_BLIT:
        if (desc.color_count > 0 || desc.has_depth || desc.input_count > 0) {
            this->error(handle, RenderResourceHandle{}, "clear and blit passes take no attachments or inputs");
            return false;
        }
        break;
    default:
        break;
    }

    out.is_raster = desc.kind == RENDER_PASS_DRAW_SCENE || desc.kind == RENDER_PASS_FULLSCREEN ||
                    (desc.kind == RENDER_PASS_CUSTOM && (desc.color_count > 0 || desc.has_depth));

    // Attachments.
    bool uses_backbuffer_depth = false;
    for (u32 i = 0; i < desc.color_count; ++i) {
        PlannedAttachment planned;
        if (!this->collect_attachment(out, handle, desc.color[i], RENDER_ASPECT_COLOR, planned, out.width, out.height)) {
            ok = false;
            continue;
        }
        if (this->plan.resources[planned.resource].kind == RENDER_RESOURCE_BACKBUFFER) {
            out.writes_backbuffer = true;
        }
        out.color[out.color_count++] = planned;
    }
    if (desc.has_depth) {
        PlannedAttachment planned;
        if (this->collect_attachment(out, handle, desc.depth, RENDER_ASPECT_DEPTH, planned, out.width, out.height)) {
            out.depth = planned;
            out.has_depth = true;
            uses_backbuffer_depth = this->plan.resources[planned.resource].kind == RENDER_RESOURCE_BACKBUFFER_DEPTH;
        } else {
            ok = false;
        }
    }

    // Inputs.
    for (u32 i = 0; i < desc.input_count; ++i) {
        const RenderResourceHandle input = desc.inputs[i];
        const u32 index = this->plan.find_resource(input);
        if (index == RENDER_GRAPH::NO_RESOURCE) {
            this->error(handle, input, "input references a removed resource");
            ok = false;
            continue;
        }
        PlannedResource& resource = this->plan.resources[index];
        if (resource.kind == RENDER_RESOURCE_BACKBUFFER) {
            this->error(handle, input, "the backbuffer cannot be read");
            ok = false;
            continue;
        }
        if (resource.kind == RENDER_RESOURCE_BACKBUFFER_DEPTH) {
            this->error(handle, input, "backbuffer_depth cannot be read");
            ok = false;
            continue;
        }
        bool attached = false;
        for (u32 c = 0; c < out.color_count; ++c) {
            attached |= out.color[c].resource == index;
        }
        attached |= out.has_depth && out.depth.resource == index;
        if (attached) {
            this->error(handle, input, "a resource cannot be sampled and attached by the same pass");
            ok = false;
            continue;
        }
        resource.used = true;
        out.inputs[out.input_count++] = index;
    }

    // Per-kind checks.
    switch (desc.kind) {
    case RENDER_PASS_DRAW_SCENE:
        if (desc.tag[0] == '\0') {
            this->error(handle, RenderResourceHandle{}, "draw_scene pass has no tag");
            ok = false;
        }
        [[fallthrough]];
    case RENDER_PASS_FULLSCREEN:
        if (desc.color_count == 0 && !desc.has_depth) {
            this->error(handle, RenderResourceHandle{}, "pass has no attachments");
            ok = false;
        }
        if (desc.kind == RENDER_PASS_FULLSCREEN && desc.shader == 0) {
            this->warning(handle, RenderResourceHandle{}, "fullscreen pass has no shader and draws nothing");
        }
        break;
    case RENDER_PASS_CUSTOM:
        if (desc.callback[0] == '\0') {
            this->warning(handle, RenderResourceHandle{}, "custom pass has no callback");
        }
        break;
    case RENDER_PASS_CLEAR: {
        const u32 target = this->plan.find_resource(desc.clear_target);
        if (target == RENDER_GRAPH::NO_RESOURCE) {
            this->error(handle, desc.clear_target, "clear pass has no target or it was removed");
            ok = false;
        } else if (is_backbuffer_kind(this->plan.resources[target].kind)) {
            this->error(handle, desc.clear_target, "the backbuffer can only be cleared by the pass that draws it");
            ok = false;
        } else {
            this->plan.resources[target].used = true;
            out.clear_target = target;
        }
        break;
    }
    case RENDER_PASS_BLIT: {
        const u32 src = this->plan.find_resource(desc.blit_src);
        const u32 dst = this->plan.find_resource(desc.blit_dst);
        if (src == RENDER_GRAPH::NO_RESOURCE || dst == RENDER_GRAPH::NO_RESOURCE) {
            this->error(handle, src == RENDER_GRAPH::NO_RESOURCE ? desc.blit_src : desc.blit_dst, "blit pass source or destination is missing or was removed");
            ok = false;
        } else if (is_backbuffer_kind(this->plan.resources[src].kind) || is_backbuffer_kind(this->plan.resources[dst].kind)) {
            this->error(handle, RenderResourceHandle{}, "the backbuffer cannot be blitted");
            ok = false;
        } else if (src == dst) {
            this->error(handle, desc.blit_src, "blit source and destination must differ");
            ok = false;
        } else if (this->plan.resources[src].aspect != this->plan.resources[dst].aspect) {
            this->error(handle, desc.blit_dst, "blit source and destination must have the same aspect");
            ok = false;
        } else {
            this->plan.resources[src].used = true;
            this->plan.resources[dst].used = true;
            out.blit_src = src;
            out.blit_dst = dst;
        }
        break;
    }
    default:
        break;
    }

    // The backbuffer pass runs through the target's fixed render pass.
    if (out.writes_backbuffer) {
        if (out.color_count != 1 || out.color[0].load != RENDER_LOAD_CLEAR || out.color[0].store != RENDER_STORE_STORE) {
            this->error(handle, this->graph.backbuffer, "the backbuffer pass must have the backbuffer as its only color attachment, cleared and stored");
            ok = false;
        }
        if (out.has_depth && (!uses_backbuffer_depth || out.depth.load != RENDER_LOAD_CLEAR || out.depth.store != RENDER_STORE_DONT_CARE)) {
            this->error(handle, this->graph.backbuffer_depth, "the backbuffer pass's depth must be backbuffer_depth, cleared and not stored");
            ok = false;
        }
    } else if (uses_backbuffer_depth) {
        this->error(handle, this->graph.backbuffer_depth, "backbuffer_depth belongs to the pass that draws the backbuffer");
        ok = false;
    }

    return ok;
}

bool Compiler::collect_passes() {
    u32 writers = 0;
    for (const RenderPassHandle handle : this->graph.order) {
        const RenderPassDesc* desc = this->graph.pass(handle);
        if (desc == nullptr || !desc->enabled) {
            continue;
        }
        PlannedPass pass;
        const bool ok = this->collect_pass(handle, *desc, pass);
        if (ok && pass.writes_backbuffer) {
            this->plan.backbuffer_pass = static_cast<u32>(this->plan.passes.count);
            writers += 1;
        }
        this->plan.passes.push(pass);
    }
    if (writers == 0) {
        this->error(RenderPassHandle{}, this->graph.backbuffer, "no enabled pass writes the backbuffer");
    } else if (writers > 1) {
        this->error(RenderPassHandle{}, this->graph.backbuffer, "more than one pass writes the backbuffer");
    }
    return this->ok;
}

void Compiler::compute_usage() {
    for (PlannedPass& pass : this->plan.passes) {
        for (u32 i = 0; i < pass.color_count; ++i) {
            this->plan.resources[pass.color[i].resource].usage |= RENDER_USAGE_COLOR;
        }
        if (pass.has_depth) {
            this->plan.resources[pass.depth.resource].usage |= RENDER_USAGE_DEPTH;
        }
        for (u32 i = 0; i < pass.input_count; ++i) {
            this->plan.resources[pass.inputs[i]].usage |= RENDER_USAGE_SAMPLED;
        }
        if (pass.clear_target != RENDER_GRAPH::NO_RESOURCE) {
            this->plan.resources[pass.clear_target].usage |= RENDER_USAGE_TRANSFER_DST;
        }
        if (pass.blit_src != RENDER_GRAPH::NO_RESOURCE) {
            this->plan.resources[pass.blit_src].usage |= RENDER_USAGE_TRANSFER_SRC;
            this->plan.resources[pass.blit_dst].usage |= RENDER_USAGE_TRANSFER_DST;
        }
    }
}

// Walks the passes backwards so every attachment learns what the resource
// is used for next and ends the pass in that state when it is a read.
void Compiler::choose_finals() {
    TemporalAllocator temp = TemporalAllocator::create();
    const usz count = this->plan.resources.count;
    RenderResourceState* next = temp.allocate_array<RenderResourceState>(count);
    for (usz i = 0; i < count; ++i) {
        next[i] = RENDER_STATE_UNDEFINED;
    }

    for (usz p = this->plan.passes.count; p > 0; --p) {
        PlannedPass& pass = this->plan.passes[p - 1];
        PlannedAttachment* attachments[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS + 1];
        u32 attachment_count = 0;
        for (u32 i = 0; i < pass.color_count; ++i) {
            attachments[attachment_count++] = &pass.color[i];
        }
        if (pass.has_depth) {
            attachments[attachment_count++] = &pass.depth;
        }
        for (u32 i = 0; i < attachment_count; ++i) {
            PlannedAttachment& attachment = *attachments[i];
            const PlannedResource& resource = this->plan.resources[attachment.resource];
            if (pass.writes_backbuffer && is_backbuffer_kind(resource.kind)) {
                attachment.final = RENDER_STATE_EXTERNAL;
            } else if (is_read_state(next[attachment.resource])) {
                if (attachment.store == RENDER_STORE_DONT_CARE) {
                    this->warning(pass.handle, resource.handle, "attachment is read later but not stored");
                }
                attachment.final = next[attachment.resource];
            } else {
                attachment.final = attachment_state(resource.aspect);
            }
        }
        // Now this pass's own needs are what earlier passes see next.
        for (u32 i = 0; i < attachment_count; ++i) {
            next[attachments[i]->resource] = attachment_state(this->plan.resources[attachments[i]->resource].aspect);
        }
        for (u32 i = 0; i < pass.input_count; ++i) {
            next[pass.inputs[i]] = RENDER_STATE_SAMPLED;
        }
        if (pass.clear_target != RENDER_GRAPH::NO_RESOURCE) {
            next[pass.clear_target] = RENDER_STATE_TRANSFER_DST;
        }
        if (pass.blit_src != RENDER_GRAPH::NO_RESOURCE) {
            next[pass.blit_src] = RENDER_STATE_TRANSFER_SRC;
            next[pass.blit_dst] = RENDER_STATE_TRANSFER_DST;
        }
    }
}

// Walks the passes forwards from each resource's frame_start_state. Without
// `record` it only finds the end states; with it, it fills every pass's
// `before` requirements and every attachment's `previous` / `initial`.
void Compiler::walk(const bool record) {
    TemporalAllocator temp = TemporalAllocator::create();
    const usz count = this->plan.resources.count;
    RenderResourceState* state = temp.allocate_array<RenderResourceState>(count);
    bool* written = temp.allocate_array<bool>(count);
    for (usz i = 0; i < count; ++i) {
        state[i] = this->plan.resources[i].frame_start_state;
        written[i] = false;
    }

    for (PlannedPass& pass : this->plan.passes) {
        if (record) {
            pass.before_count = 0;
        }
        auto require = [&](const u32 resource, const RenderResourceState wanted) {
            if (record) {
                PlannedRequirement& requirement = pass.before[pass.before_count++];
                requirement.resource = resource;
                requirement.state = wanted;
            }
            state[resource] = wanted;
        };

        for (u32 i = 0; i < pass.input_count; ++i) {
            const u32 resource = pass.inputs[i];
            if (record && !written[resource]) {
                this->warning(pass.handle, this->plan.resources[resource].handle, "input is read before any pass writes it this frame");
            }
            require(resource, RENDER_STATE_SAMPLED);
        }
        if (pass.clear_target != RENDER_GRAPH::NO_RESOURCE) {
            require(pass.clear_target, RENDER_STATE_TRANSFER_DST);
            written[pass.clear_target] = true;
        }
        if (pass.blit_src != RENDER_GRAPH::NO_RESOURCE) {
            if (record && !written[pass.blit_src]) {
                this->warning(pass.handle, this->plan.resources[pass.blit_src].handle, "blit source is read before any pass writes it this frame");
            }
            require(pass.blit_src, RENDER_STATE_TRANSFER_SRC);
            require(pass.blit_dst, RENDER_STATE_TRANSFER_DST);
            written[pass.blit_dst] = true;
        }

        PlannedAttachment* attachments[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS + 1];
        u32 attachment_count = 0;
        for (u32 i = 0; i < pass.color_count; ++i) {
            attachments[attachment_count++] = &pass.color[i];
        }
        if (pass.has_depth) {
            attachments[attachment_count++] = &pass.depth;
        }
        for (u32 i = 0; i < attachment_count; ++i) {
            PlannedAttachment& attachment = *attachments[i];
            const u32 resource = attachment.resource;
            if (record) {
                attachment.previous = state[resource];
                attachment.initial = attachment.load == RENDER_LOAD_LOAD ? state[resource] : RENDER_STATE_UNDEFINED;
                if (attachment.load == RENDER_LOAD_LOAD && !written[resource] && !is_backbuffer_kind(this->plan.resources[resource].kind)) {
                    this->warning(pass.handle, this->plan.resources[resource].handle, "attachment loads contents no pass wrote this frame");
                }
            }
            state[resource] = attachment.final;
            written[resource] = true;
        }
    }

    for (usz i = 0; i < count; ++i) {
        this->plan.resources[i].frame_end_state = state[i];
    }
}

// Keys need the states the walks decided, so they come last.
void Compiler::collect_keys() {
    for (PlannedPass& pass : this->plan.passes) {
        if (!pass.is_raster) {
            continue;
        }
        pass.compat_key = pass.writes_backbuffer ? RENDER_GRAPH::target_compat_key(this->backbuffer) : RENDER_GRAPH::pass_compat_key(pass);
        bool known = false;
        for (const u64 key : this->plan.compat_keys) {
            known |= key == pass.compat_key;
        }
        if (!known) {
            this->plan.compat_keys.push(pass.compat_key);
        }
    }
}

} // namespace

bool RENDER_GRAPH::compile(const RenderGraph& graph, const RenderBackbufferInfo& backbuffer, RenderGraphPlan& out, DynamicArray<RenderDiagnostic>* diagnostics) {
    out.clear();
    out.backbuffer = backbuffer;
    out.graph_version = graph.version;

    Compiler compiler{graph, backbuffer, out, diagnostics};
    if (backbuffer.color_format == RENDER_FORMAT_UNDEFINED || backbuffer.width == 0 || backbuffer.height == 0) {
        compiler.error(RenderPassHandle{}, graph.backbuffer, "the backbuffer has no format or size");
    }
    compiler.collect_resources();
    compiler.collect_passes();
    if (!compiler.ok) {
        out.clear();
        return false;
    }

    compiler.compute_usage();
    compiler.choose_finals();

    // First walk from nothing to learn where each resource ends the frame,
    // which is also where the next frame finds it.
    for (PlannedResource& resource : out.resources) {
        resource.frame_start_state = RENDER_STATE_UNDEFINED;
    }
    compiler.walk(false);
    for (PlannedResource& resource : out.resources) {
        resource.frame_start_state = is_backbuffer_kind(resource.kind) ? RENDER_STATE_UNDEFINED : resource.frame_end_state;
    }
    // Second walk from there records what the backend has to do.
    compiler.walk(true);
    compiler.collect_keys();
    return true;
}
