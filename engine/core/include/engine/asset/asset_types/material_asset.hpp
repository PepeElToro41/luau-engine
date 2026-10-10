#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/defines.hpp"
#include "engine/templates/dynamic_array.hpp"

// A material asset (ASSET_TYPE::MATERIAL): a `.material` text file naming a
// shader and the values of its material interface (set 2: the block
// members, the textures, the samplers). docs/asset_format.md "Material
// files" is the specification.
//
//     guid = e26f6a98c55cbe22179a52dd06b33f96
//     shader = textured                   # render/<name>.slang, as SHADER_LIBRARY::load takes it
//
//     [params]                            # members of the material block, by name
//     tint = 1 0.5 0.15 1                 # 1..16 numbers: scalar, vector or column-major matrix
//     tiles = 3
//
//     [textures]                          # Texture2D of set 2, by name: a texture asset GUID or none
//     albedo = 179a52dd06b33f96e26f6a98c55cbe22
//
//     [samplers]                          # SamplerState of set 2, by name
//     albedo_sampler = linear repeat anisotropy=8
//
// The file knows nothing about the shader's types: a value is a list of
// numbers, and whoever applies it (MATERIAL::load in the graphics module)
// checks the count and the member's scalar type against the shader's
// reflection and reports mismatches. Names that the shader does not declare
// are reported too, not silently dropped, so a renamed uniform is noticed.
//
// A sampler is words: `linear` / `nearest` set all three filters,
// `repeat` / `clamp` / `mirror` all three address modes, and `min=`,
// `mag=`, `mip=`, `u=`, `v=`, `w=`, `anisotropy=` override one. Defaults
// are trilinear, repeat, no anisotropy.
//
//     MaterialAsset material;
//     u32 line = 0;
//     if (MATERIAL_ASSET::parse(text, size, material, &line) == MATERIAL_PARSE_OK) { ... }
//
//     DynamicArray<char> out;
//     MATERIAL_ASSET::write(material, out);        // the text back, canonical layout
//
// Core has no GPU types, so the sampler description here is the file's
// vocabulary; the graphics module maps it to a GpuSamplerDesc.

namespace MATERIAL_ASSET {

constexpr u32 MAX_PARAMS = 32;
constexpr u32 MAX_TEXTURES = 8;
constexpr u32 MAX_SAMPLERS = 4;
// Numbers one param holds at most: a 4x4 matrix.
constexpr u32 MAX_VALUES = 16;
constexpr usz NAME_CAPACITY = 64;

} // namespace MATERIAL_ASSET

enum MaterialFilter : u8 {
    MATERIAL_FILTER_LINEAR = 0,
    MATERIAL_FILTER_NEAREST = 1,
};

enum MaterialAddress : u8 {
    MATERIAL_ADDRESS_REPEAT = 0,
    MATERIAL_ADDRESS_CLAMP = 1,
    MATERIAL_ADDRESS_MIRROR = 2,
};

struct MaterialSamplerDesc {
    MaterialFilter min_filter = MATERIAL_FILTER_LINEAR;
    MaterialFilter mag_filter = MATERIAL_FILTER_LINEAR;
    MaterialFilter mip_filter = MATERIAL_FILTER_LINEAR;
    MaterialAddress address_u = MATERIAL_ADDRESS_REPEAT;
    MaterialAddress address_v = MATERIAL_ADDRESS_REPEAT;
    MaterialAddress address_w = MATERIAL_ADDRESS_REPEAT;
    // 0 disables anisotropic filtering.
    f32 max_anisotropy = 0.0f;

    friend bool operator==(const MaterialSamplerDesc& a, const MaterialSamplerDesc& b) {
        return a.min_filter == b.min_filter && a.mag_filter == b.mag_filter && a.mip_filter == b.mip_filter && a.address_u == b.address_u &&
               a.address_v == b.address_v && a.address_w == b.address_w && a.max_anisotropy == b.max_anisotropy;
    }
    friend bool operator!=(const MaterialSamplerDesc& a, const MaterialSamplerDesc& b) { return !(a == b); }
};

// One `[params]` line: the member's name and its numbers as written. The
// member's type (float or int, scalar, vector or matrix) is the shader's;
// `count` must match its component count when applied.
struct MaterialParam {
    char name[MATERIAL_ASSET::NAME_CAPACITY] = {};
    f64 values[MATERIAL_ASSET::MAX_VALUES] = {};
    u32 count = 0;
};

// One `[textures]` line. A null `texture` is `none`: the renderer's white
// texture.
struct MaterialTextureRef {
    char name[MATERIAL_ASSET::NAME_CAPACITY] = {};
    AssetGuid texture;
};

// One `[samplers]` line.
struct MaterialSamplerRef {
    char name[MATERIAL_ASSET::NAME_CAPACITY] = {};
    MaterialSamplerDesc sampler;
};

// A parsed (or to-be-written) material. Plain data, no storage to free.
struct MaterialAsset {
    AssetGuid guid;
    char shader[MATERIAL_ASSET::NAME_CAPACITY] = {};
    MaterialParam params[MATERIAL_ASSET::MAX_PARAMS] = {};
    u32 param_count = 0;
    MaterialTextureRef textures[MATERIAL_ASSET::MAX_TEXTURES] = {};
    u32 texture_count = 0;
    MaterialSamplerRef samplers[MATERIAL_ASSET::MAX_SAMPLERS] = {};
    u32 sampler_count = 0;

    void clear() { *this = MaterialAsset(); }

    // Lookups by name; nullptr when absent.
    const MaterialParam* find_param(const char* name) const;
    const MaterialTextureRef* find_texture(const char* name) const;
    const MaterialSamplerRef* find_sampler(const char* name) const;

    // Set a param / texture / sampler by name, replacing an existing one of
    // that name or appending. False when the table is full, the name does
    // not fit or `count` exceeds MAX_VALUES.
    bool set_param(const char* name, const f64* values, u32 count);
    bool set_param(const char* name, f64 value) { return this->set_param(name, &value, 1); }
    bool set_texture(const char* name, const AssetGuid& texture);
    bool set_sampler(const char* name, const MaterialSamplerDesc& sampler);
};

enum MaterialParseError {
    MATERIAL_PARSE_OK = 0,
    MATERIAL_PARSE_BAD_SYNTAX,      // a line that is not blank, a comment, a [section] or `key = value`
    MATERIAL_PARSE_MISSING_GUID,    // no top-level guid
    MATERIAL_PARSE_BAD_GUID,        // guid is not 32 hex digits, or a texture GUID is not
    MATERIAL_PARSE_MISSING_SHADER,  // no top-level shader
    MATERIAL_PARSE_UNKNOWN_KEY,     // a top-level key or section this version does not know
    MATERIAL_PARSE_BAD_VALUE,       // a param that is not numbers, a sampler word that is not one, a name that does not fit
    MATERIAL_PARSE_TOO_MANY,        // more params, textures or samplers than the tables hold
    MATERIAL_PARSE_DUPLICATE,       // a name given twice in one section
};

namespace MATERIAL_ASSET {

const char* parse_error_name(MaterialParseError error);

// Parses `size` bytes of material text into `out`, which is cleared first.
// On error `out` is whatever was read so far and `out_line` (when given)
// is the offending line, 0 when the problem is a missing key.
MaterialParseError parse(const char* text, usz size, MaterialAsset& out, u32* out_line = nullptr);
// The same from a loaded text asset's payload: the TEXT chunk's bytes.
MaterialParseError parse(const AssetView& file, const void* payload, usz size, MaterialAsset& out, u32* out_line = nullptr);

// Appends `material` as text to `out` (no terminator) in the canonical
// layout: guid, shader, then the three sections, each omitted when empty.
// Numbers are written with the fewest digits that read back to the same
// f32, so a value that came from a float round-trips exactly.
void write(const MaterialAsset& material, DynamicArray<char>& out);
// Writes `material` to `path`, replacing any existing file. False (with a
// message on stderr) when the guid is null or the file cannot be written.
bool write_file(const MaterialAsset& material, const char* path);

// The sampler words of a value; false (with `out` partly set) on a word
// that is not one.
bool parse_sampler(const char* value, usz length, MaterialSamplerDesc* out);
// The words that parse back to `sampler`, terminated, into `capacity`
// characters; the shortest form (`linear repeat` for the default).
void format_sampler(const MaterialSamplerDesc& sampler, char* out, usz capacity);

} // namespace MATERIAL_ASSET
