#include "engine/gpu/vertex_input.hpp"

#include "engine/gpu/asset_formats.hpp"

#include <cstdio>
#include <cstring>

// --- GpuMeshLayout -----------------------------------------------------------------

void GpuMeshLayout::set(const VertexStreamDesc* streams, u32 stream_count, const VertexAttributeDesc* attributes, u32 attribute_count) {
    if (stream_count > MESH_ASSET::MAX_STREAMS) {
        stream_count = MESH_ASSET::MAX_STREAMS;
    }
    if (attribute_count > MESH_ASSET::MAX_ATTRIBUTES) {
        attribute_count = MESH_ASSET::MAX_ATTRIBUTES;
    }
    memcpy(this->streams, streams, stream_count * sizeof(VertexStreamDesc));
    this->stream_count = stream_count;
    memcpy(this->attributes, attributes, attribute_count * sizeof(VertexAttributeDesc));
    this->attribute_count = attribute_count;
    this->hash = VERTEX_LAYOUT::hash(this->streams, this->stream_count, this->attributes, this->attribute_count);
}

const VertexAttributeDesc* GpuMeshLayout::find_location(const u32 location) const {
    for (u32 i = 0; i < this->attribute_count; ++i) {
        const VertexAttributeDesc& attribute = this->attributes[i];
        if (VERTEX_LAYOUT::location(attribute.semantic, attribute.semantic_index) == location) {
            return &attribute;
        }
    }
    return nullptr;
}

// --- VERTEX_INPUT -------------------------------------------------------------------

static const char* location_name(const u32 location, char* buffer, const usz size) {
    for (u32 semantic = VERTEX_SEMANTIC_POSITION; semantic <= VERTEX_SEMANTIC_WEIGHTS; ++semantic) {
        for (u32 index = 0; index < VERTEX_LAYOUT::MAX_TEXCOORDS; ++index) {
            if (VERTEX_LAYOUT::location(semantic, index) == location) {
                snprintf(buffer, size, "%s%u", VERTEX_LAYOUT::semantic_name(semantic), index);
                return buffer;
            }
        }
    }
    snprintf(buffer, size, "location %u", location);
    return buffer;
}

bool VERTEX_INPUT::build(const ShaderReflection& vertex, const GpuMeshLayout& mesh, GraphicsPipelineDesc& desc, char* error, const usz error_size) {
    for (u32 s = 0; s < mesh.stream_count; ++s) {
        if (desc.add_binding(mesh.streams[s].stride) == 0xffffffffu) {
            snprintf(error, error_size, "mesh has more vertex streams than a pipeline allows");
            return false;
        }
    }
    for (u32 i = 0; i < vertex.input_count; ++i) {
        const ReflectedInput& input = vertex.inputs[i];
        const VertexAttributeDesc* attribute = mesh.find_location(input.location);
        char name[32];
        if (attribute == nullptr) {
            snprintf(error, error_size, "mesh has no %s attribute, which the shader reads at location %u",
                location_name(input.location, name, sizeof(name)), input.location);
            return false;
        }
        const VkFormat format = ASSET_FORMATS::vertex_format(attribute->format);
        if (format == VK_FORMAT_UNDEFINED) {
            snprintf(error, error_size, "mesh attribute %s has an unknown format", location_name(input.location, name, sizeof(name)));
            return false;
        }
        const bool integer_input = input.scalar == REFLECT_SCALAR_INT || input.scalar == REFLECT_SCALAR_UINT;
        if (integer_input != ASSET_FORMATS::vertex_format_is_integer(attribute->format)) {
            snprintf(error, error_size, "mesh attribute %s is %s but the shader input is %s",
                location_name(input.location, name, sizeof(name)), VERTEX_FORMAT::name(attribute->format), SHADER_REFLECT::scalar_name(input.scalar));
            return false;
        }
        if (!desc.add_attribute(input.location, format, attribute->offset, attribute->stream)) {
            snprintf(error, error_size, "shader reads more vertex attributes than a pipeline allows");
            return false;
        }
    }
    return true;
}
