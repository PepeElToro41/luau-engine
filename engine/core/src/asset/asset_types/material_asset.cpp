#include "engine/asset/asset_types/material_asset.hpp"

#include "engine/platform/file.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// --- Lookups ------------------------------------------------------------------

namespace {

template <typename T>
const T* find_by_name(const T* entries, const u32 count, const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (u32 i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) {
            return entries + i;
        }
    }
    return nullptr;
}

template <typename T>
T* find_or_append(T* entries, u32* count, const u32 capacity, const char* name) {
    if (name == nullptr || name[0] == '\0' || strlen(name) >= MATERIAL_ASSET::NAME_CAPACITY) {
        return nullptr;
    }
    if (T* existing = const_cast<T*>(find_by_name(entries, *count, name))) {
        return existing;
    }
    if (*count >= capacity) {
        return nullptr;
    }
    T* entry = entries + (*count)++;
    *entry = T();
    strncpy(entry->name, name, MATERIAL_ASSET::NAME_CAPACITY - 1);
    return entry;
}

} // namespace

const MaterialParam* MaterialAsset::find_param(const char* name) const {
    return find_by_name(this->params, this->param_count, name);
}

const MaterialTextureRef* MaterialAsset::find_texture(const char* name) const {
    return find_by_name(this->textures, this->texture_count, name);
}

const MaterialSamplerRef* MaterialAsset::find_sampler(const char* name) const {
    return find_by_name(this->samplers, this->sampler_count, name);
}

bool MaterialAsset::set_param(const char* name, const f64* values, const u32 count) {
    if (count > MATERIAL_ASSET::MAX_VALUES || (values == nullptr && count != 0)) {
        return false;
    }
    MaterialParam* param = find_or_append(this->params, &this->param_count, MATERIAL_ASSET::MAX_PARAMS, name);
    if (param == nullptr) {
        return false;
    }
    memset(param->values, 0, sizeof(param->values));
    for (u32 i = 0; i < count; ++i) {
        param->values[i] = values[i];
    }
    param->count = count;
    return true;
}

bool MaterialAsset::set_texture(const char* name, const AssetGuid& texture) {
    MaterialTextureRef* ref = find_or_append(this->textures, &this->texture_count, MATERIAL_ASSET::MAX_TEXTURES, name);
    if (ref == nullptr) {
        return false;
    }
    ref->texture = texture;
    return true;
}

bool MaterialAsset::set_sampler(const char* name, const MaterialSamplerDesc& sampler) {
    MaterialSamplerRef* ref = find_or_append(this->samplers, &this->sampler_count, MATERIAL_ASSET::MAX_SAMPLERS, name);
    if (ref == nullptr) {
        return false;
    }
    ref->sampler = sampler;
    return true;
}

// --- Samplers -----------------------------------------------------------------

namespace {

bool parse_filter(const char* word, const usz length, MaterialFilter* out) {
    if (TEXT_ASSET::word_is(word, length, "linear")) {
        *out = MATERIAL_FILTER_LINEAR;
        return true;
    }
    if (TEXT_ASSET::word_is(word, length, "nearest")) {
        *out = MATERIAL_FILTER_NEAREST;
        return true;
    }
    return false;
}

bool parse_address(const char* word, const usz length, MaterialAddress* out) {
    if (TEXT_ASSET::word_is(word, length, "repeat")) {
        *out = MATERIAL_ADDRESS_REPEAT;
        return true;
    }
    if (TEXT_ASSET::word_is(word, length, "clamp")) {
        *out = MATERIAL_ADDRESS_CLAMP;
        return true;
    }
    if (TEXT_ASSET::word_is(word, length, "mirror")) {
        *out = MATERIAL_ADDRESS_MIRROR;
        return true;
    }
    return false;
}

const char* filter_name(const MaterialFilter filter) {
    return filter == MATERIAL_FILTER_NEAREST ? "nearest" : "linear";
}

const char* address_name(const MaterialAddress address) {
    switch (address) {
    case MATERIAL_ADDRESS_CLAMP:
        return "clamp";
    case MATERIAL_ADDRESS_MIRROR:
        return "mirror";
    default:
        return "repeat";
    }
}

} // namespace

bool MATERIAL_ASSET::parse_sampler(const char* value, const usz length, MaterialSamplerDesc* out) {
    *out = MaterialSamplerDesc();
    usz offset = 0;
    const char* word = nullptr;
    usz word_length = 0;
    while (TEXT_ASSET::next_word(value, length, &offset, &word, &word_length)) {
        MaterialFilter filter;
        MaterialAddress address;
        if (parse_filter(word, word_length, &filter)) {
            out->min_filter = out->mag_filter = out->mip_filter = filter;
            continue;
        }
        if (parse_address(word, word_length, &address)) {
            out->address_u = out->address_v = out->address_w = address;
            continue;
        }
        const char* equals = static_cast<const char*>(memchr(word, '=', word_length));
        if (equals == nullptr) {
            return false;
        }
        const char* key = word;
        const usz key_length = static_cast<usz>(equals - word);
        const char* argument = equals + 1;
        const usz argument_length = static_cast<usz>(word + word_length - argument);
        if (TEXT_ASSET::word_is(key, key_length, "min")) {
            if (!parse_filter(argument, argument_length, &out->min_filter)) {
                return false;
            }
        } else if (TEXT_ASSET::word_is(key, key_length, "mag")) {
            if (!parse_filter(argument, argument_length, &out->mag_filter)) {
                return false;
            }
        } else if (TEXT_ASSET::word_is(key, key_length, "mip")) {
            if (!parse_filter(argument, argument_length, &out->mip_filter)) {
                return false;
            }
        } else if (TEXT_ASSET::word_is(key, key_length, "u")) {
            if (!parse_address(argument, argument_length, &out->address_u)) {
                return false;
            }
        } else if (TEXT_ASSET::word_is(key, key_length, "v")) {
            if (!parse_address(argument, argument_length, &out->address_v)) {
                return false;
            }
        } else if (TEXT_ASSET::word_is(key, key_length, "w")) {
            if (!parse_address(argument, argument_length, &out->address_w)) {
                return false;
            }
        } else if (TEXT_ASSET::word_is(key, key_length, "anisotropy")) {
            f64 number = 0.0;
            usz count = 0;
            if (!TEXT_ASSET::parse_numbers(argument, argument_length, &number, 1, &count) || count != 1 || number < 0.0) {
                return false;
            }
            out->max_anisotropy = static_cast<f32>(number);
        } else {
            return false;
        }
    }
    return true;
}

void MATERIAL_ASSET::format_sampler(const MaterialSamplerDesc& sampler, char* out, const usz capacity) {
    if (capacity == 0) {
        return;
    }
    usz length = 0;
    const auto append = [&](const char* word) {
        if (length >= capacity) {
            return;
        }
        const int written = snprintf(out + length, capacity - length, "%s%s", length > 0 ? " " : "", word);
        length += written > 0 ? static_cast<usz>(written) : 0;
    };
    const auto append_pair = [&](const char* key, const char* value) {
        char word[48];
        snprintf(word, sizeof(word), "%s=%s", key, value);
        append(word);
    };
    if (sampler.min_filter == sampler.mag_filter && sampler.mag_filter == sampler.mip_filter) {
        append(filter_name(sampler.min_filter));
    } else {
        append_pair("min", filter_name(sampler.min_filter));
        append_pair("mag", filter_name(sampler.mag_filter));
        append_pair("mip", filter_name(sampler.mip_filter));
    }
    if (sampler.address_u == sampler.address_v && sampler.address_v == sampler.address_w) {
        append(address_name(sampler.address_u));
    } else {
        append_pair("u", address_name(sampler.address_u));
        append_pair("v", address_name(sampler.address_v));
        append_pair("w", address_name(sampler.address_w));
    }
    if (sampler.max_anisotropy > 0.0f) {
        char number[32];
        snprintf(number, sizeof(number), "%g", static_cast<f64>(sampler.max_anisotropy));
        append_pair("anisotropy", number);
    }
    if (length >= capacity) {
        out[capacity - 1] = '\0';
    }
}

// --- Parsing ------------------------------------------------------------------

const char* MATERIAL_ASSET::parse_error_name(const MaterialParseError error) {
    switch (error) {
    case MATERIAL_PARSE_OK:
        return "ok";
    case MATERIAL_PARSE_BAD_SYNTAX:
        return "malformed line";
    case MATERIAL_PARSE_MISSING_GUID:
        return "missing guid";
    case MATERIAL_PARSE_BAD_GUID:
        return "malformed guid";
    case MATERIAL_PARSE_MISSING_SHADER:
        return "missing shader";
    case MATERIAL_PARSE_UNKNOWN_KEY:
        return "unknown key or section";
    case MATERIAL_PARSE_BAD_VALUE:
        return "bad value";
    case MATERIAL_PARSE_TOO_MANY:
        return "too many entries";
    case MATERIAL_PARSE_DUPLICATE:
        return "duplicate name";
    }
    return "unknown error";
}

namespace {

MaterialParseError fail(const MaterialParseError error, const u32 line, u32* out_line) {
    if (out_line != nullptr) {
        *out_line = line;
    }
    return error;
}

} // namespace

MaterialParseError MATERIAL_ASSET::parse(const char* text, const usz size, MaterialAsset& out, u32* out_line) {
    out.clear();
    if (out_line != nullptr) {
        *out_line = 0;
    }
    if (text == nullptr && size != 0) {
        return MATERIAL_PARSE_BAD_SYNTAX;
    }
    bool has_guid = false;
    TextAssetCursor cursor(text, size);
    TextAssetEntry entry;
    char name[NAME_CAPACITY];
    while (cursor.next(&entry)) {
        if (!TEXT_ASSET::copy_span(entry.key, entry.key_length, name, sizeof(name))) {
            return fail(MATERIAL_PARSE_BAD_VALUE, entry.line, out_line);
        }
        if (entry.in_top_level()) {
            if (entry.key_is("guid")) {
                if (has_guid) {
                    return fail(MATERIAL_PARSE_DUPLICATE, entry.line, out_line);
                }
                if (!TEXT_ASSET::parse_guid(entry.value, entry.value_length, &out.guid)) {
                    return fail(MATERIAL_PARSE_BAD_GUID, entry.line, out_line);
                }
                has_guid = true;
            } else if (entry.key_is("shader")) {
                if (out.shader[0] != '\0') {
                    return fail(MATERIAL_PARSE_DUPLICATE, entry.line, out_line);
                }
                if (entry.value_length == 0 || !TEXT_ASSET::copy_span(entry.value, entry.value_length, out.shader, sizeof(out.shader))) {
                    return fail(MATERIAL_PARSE_BAD_VALUE, entry.line, out_line);
                }
            } else {
                return fail(MATERIAL_PARSE_UNKNOWN_KEY, entry.line, out_line);
            }
        } else if (entry.section_is("params")) {
            if (out.find_param(name) != nullptr) {
                return fail(MATERIAL_PARSE_DUPLICATE, entry.line, out_line);
            }
            f64 values[MAX_VALUES];
            usz count = 0;
            if (!TEXT_ASSET::parse_numbers(entry.value, entry.value_length, values, MAX_VALUES, &count) || count == 0) {
                return fail(MATERIAL_PARSE_BAD_VALUE, entry.line, out_line);
            }
            if (out.param_count >= MAX_PARAMS) {
                return fail(MATERIAL_PARSE_TOO_MANY, entry.line, out_line);
            }
            out.set_param(name, values, static_cast<u32>(count));
        } else if (entry.section_is("textures")) {
            if (out.find_texture(name) != nullptr) {
                return fail(MATERIAL_PARSE_DUPLICATE, entry.line, out_line);
            }
            AssetGuid texture;
            if (!entry.value_is("none") && !TEXT_ASSET::parse_guid(entry.value, entry.value_length, &texture)) {
                return fail(MATERIAL_PARSE_BAD_GUID, entry.line, out_line);
            }
            if (out.texture_count >= MAX_TEXTURES) {
                return fail(MATERIAL_PARSE_TOO_MANY, entry.line, out_line);
            }
            out.set_texture(name, texture);
        } else if (entry.section_is("samplers")) {
            if (out.find_sampler(name) != nullptr) {
                return fail(MATERIAL_PARSE_DUPLICATE, entry.line, out_line);
            }
            MaterialSamplerDesc sampler;
            if (!parse_sampler(entry.value, entry.value_length, &sampler)) {
                return fail(MATERIAL_PARSE_BAD_VALUE, entry.line, out_line);
            }
            if (out.sampler_count >= MAX_SAMPLERS) {
                return fail(MATERIAL_PARSE_TOO_MANY, entry.line, out_line);
            }
            out.set_sampler(name, sampler);
        } else {
            return fail(MATERIAL_PARSE_UNKNOWN_KEY, entry.line, out_line);
        }
    }
    if (cursor.error != nullptr) {
        return fail(MATERIAL_PARSE_BAD_SYNTAX, cursor.line, out_line);
    }
    if (!has_guid) {
        return MATERIAL_PARSE_MISSING_GUID;
    }
    if (out.shader[0] == '\0') {
        return MATERIAL_PARSE_MISSING_SHADER;
    }
    return MATERIAL_PARSE_OK;
}

MaterialParseError MATERIAL_ASSET::parse(const AssetView& file, const void* payload, const usz size, MaterialAsset& out, u32* out_line) {
    if (!file.is_ok() || file.header->type != ASSET_TYPE::MATERIAL || !file.is_text()) {
        out.clear();
        if (out_line != nullptr) {
            *out_line = 0;
        }
        return MATERIAL_PARSE_BAD_SYNTAX;
    }
    const MaterialParseError error = parse(static_cast<const char*>(payload), size, out, out_line);
    if (error == MATERIAL_PARSE_OK && out.guid != file.header->guid) {
        // The text names another asset than the prelude it was registered
        // under; the provider would have refreshed, so this is a torn file.
        return MATERIAL_PARSE_BAD_GUID;
    }
    return error;
}

// --- Writing ------------------------------------------------------------------

namespace {

void append_text(DynamicArray<char>& out, const char* text) {
    for (const char* c = text; *c != '\0'; ++c) {
        out.push(*c);
    }
}

// The fewest significant digits that read back to the same f32.
void append_number(DynamicArray<char>& out, const f64 value) {
    const f32 single = static_cast<f32>(value);
    char buffer[32];
    for (int precision = 1; precision <= 9; ++precision) {
        snprintf(buffer, sizeof(buffer), "%.*g", precision, static_cast<f64>(single));
        if (strtof(buffer, nullptr) == single) {
            break;
        }
    }
    append_text(out, buffer);
}

} // namespace

void MATERIAL_ASSET::write(const MaterialAsset& material, DynamicArray<char>& out) {
    char guid[TEXT_ASSET::GUID_TEXT_CAPACITY];
    TEXT_ASSET::format_guid(material.guid, guid);
    append_text(out, "guid = ");
    append_text(out, guid);
    append_text(out, "\nshader = ");
    append_text(out, material.shader);
    append_text(out, "\n");

    if (material.param_count > 0) {
        append_text(out, "\n[params]\n");
        for (u32 i = 0; i < material.param_count; ++i) {
            const MaterialParam& param = material.params[i];
            append_text(out, param.name);
            append_text(out, " =");
            for (u32 v = 0; v < param.count; ++v) {
                append_text(out, " ");
                append_number(out, param.values[v]);
            }
            append_text(out, "\n");
        }
    }
    if (material.texture_count > 0) {
        append_text(out, "\n[textures]\n");
        for (u32 i = 0; i < material.texture_count; ++i) {
            const MaterialTextureRef& ref = material.textures[i];
            append_text(out, ref.name);
            append_text(out, " = ");
            if (ref.texture.is_null()) {
                append_text(out, "none");
            } else {
                TEXT_ASSET::format_guid(ref.texture, guid);
                append_text(out, guid);
            }
            append_text(out, "\n");
        }
    }
    if (material.sampler_count > 0) {
        append_text(out, "\n[samplers]\n");
        for (u32 i = 0; i < material.sampler_count; ++i) {
            const MaterialSamplerRef& ref = material.samplers[i];
            char words[128];
            format_sampler(ref.sampler, words, sizeof(words));
            append_text(out, ref.name);
            append_text(out, " = ");
            append_text(out, words);
            append_text(out, "\n");
        }
    }
}

bool MATERIAL_ASSET::write_file(const MaterialAsset& material, const char* path) {
    if (material.guid.is_null()) {
        fprintf(stderr, "[asset] error: cannot write %s: the material has no guid\n", path != nullptr ? path : "(null)");
        return false;
    }
    DynamicArray<char> text;
    write(material, text);
    File file;
    if (!PLATFORM::file_open(&file, path, FILE_ACCESS_WRITE)) {
        fprintf(stderr, "[asset] error: cannot open %s for writing\n", path != nullptr ? path : "(null)");
        text.free();
        return false;
    }
    const bool ok = PLATFORM::file_write(file, 0, text.data, text.count);
    PLATFORM::file_close(&file);
    if (!ok) {
        fprintf(stderr, "[asset] error: writing %s failed\n", path);
    }
    text.free();
    return ok;
}
