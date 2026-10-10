#include "engine/asset/text_asset.hpp"

#include "engine/asset/asset_reader.hpp"
#include "engine/platform/file.hpp"
#include "engine/utils/hash.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// --- Types --------------------------------------------------------------------

namespace {

bool is_space(const char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

char lower(const char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Trims both ends of [text, text + length).
void trim(const char** text, usz* length) {
    while (*length > 0 && is_space(**text)) {
        ++*text;
        --*length;
    }
    while (*length > 0 && is_space((*text)[*length - 1])) {
        --*length;
    }
}

} // namespace

u32 TEXT_ASSET::type_of_path(const char* path) {
    if (path == nullptr) {
        return 0;
    }
    const usz length = strlen(path);
    const usz extension_length = strlen(MATERIAL_EXTENSION);
    if (length < extension_length) {
        return 0;
    }
    const char* extension = path + length - extension_length;
    for (usz i = 0; i < extension_length; ++i) {
        if (lower(extension[i]) != MATERIAL_EXTENSION[i]) {
            return 0;
        }
    }
    return ASSET_TYPE::MATERIAL;
}

const char* TEXT_ASSET::extension_of_type(const u32 type) {
    return type == ASSET_TYPE::MATERIAL ? MATERIAL_EXTENSION : nullptr;
}

// --- GUIDs --------------------------------------------------------------------

void TEXT_ASSET::format_guid(const AssetGuid& guid, char* out) {
    snprintf(out, GUID_TEXT_CAPACITY, "%016llx%016llx", static_cast<unsigned long long>(guid.hi), static_cast<unsigned long long>(guid.lo));
}

bool TEXT_ASSET::parse_guid(const char* text, const usz length, AssetGuid* out) {
    u64 halves[2] = {0, 0};
    usz digits = 0;
    for (usz i = 0; i < length; ++i) {
        const char c = text[i];
        if (c == '-') {
            continue;
        }
        u64 digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<u64>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<u64>(c - 'A' + 10);
        } else {
            return false;
        }
        if (digits >= GUID_TEXT_LENGTH) {
            return false;
        }
        halves[digits / 16] = (halves[digits / 16] << 4) | digit;
        ++digits;
    }
    if (digits != GUID_TEXT_LENGTH) {
        return false;
    }
    const AssetGuid guid = {halves[1], halves[0]};
    if (guid.is_null()) {
        return false;
    }
    *out = guid;
    return true;
}

// --- The line syntax ----------------------------------------------------------

bool TEXT_ASSET::word_is(const char* word, const usz length, const char* name) {
    return strlen(name) == length && memcmp(word, name, length) == 0;
}

bool TextAssetEntry::section_is(const char* name) const {
    return TEXT_ASSET::word_is(this->section, this->section_length, name);
}

bool TextAssetEntry::key_is(const char* name) const {
    return TEXT_ASSET::word_is(this->key, this->key_length, name);
}

bool TextAssetEntry::value_is(const char* text) const {
    return TEXT_ASSET::word_is(this->value, this->value_length, text);
}

bool TextAssetCursor::next(TextAssetEntry* out) {
    this->error = nullptr;
    while (this->offset < this->size) {
        // One line, without its terminator.
        const char* line_text = this->text + this->offset;
        usz line_length = 0;
        while (this->offset + line_length < this->size && line_text[line_length] != '\n') {
            ++line_length;
        }
        this->offset += line_length + (this->offset + line_length < this->size ? 1 : 0);
        this->line += 1;
        trim(&line_text, &line_length);
        if (line_length == 0 || line_text[0] == '#') {
            continue;
        }

        if (line_text[0] == '[') {
            const char* close = static_cast<const char*>(memchr(line_text, ']', line_length));
            if (close == nullptr) {
                this->error = "unterminated [section]";
                return false;
            }
            const char* rest = close + 1;
            usz rest_length = static_cast<usz>(line_text + line_length - rest);
            trim(&rest, &rest_length);
            if (rest_length != 0 && rest[0] != '#') {
                this->error = "text after the [section]";
                return false;
            }
            this->section = line_text + 1;
            this->section_length = static_cast<usz>(close - this->section);
            trim(&this->section, &this->section_length);
            if (this->section_length == 0) {
                this->error = "empty [section] name";
                return false;
            }
            continue;
        }

        const char* equals = static_cast<const char*>(memchr(line_text, '=', line_length));
        if (equals == nullptr) {
            this->error = "expected `key = value`";
            return false;
        }
        const char* key = line_text;
        usz key_length = static_cast<usz>(equals - line_text);
        trim(&key, &key_length);
        if (key_length == 0) {
            this->error = "missing key before `=`";
            return false;
        }
        const char* value = equals + 1;
        usz value_length = static_cast<usz>(line_text + line_length - value);
        trim(&value, &value_length);
        if (value_length > 0 && value[0] == '"') {
            const char* close = static_cast<const char*>(memchr(value + 1, '"', value_length - 1));
            if (close == nullptr) {
                this->error = "unterminated quoted value";
                return false;
            }
            const char* rest = close + 1;
            usz rest_length = static_cast<usz>(value + value_length - rest);
            trim(&rest, &rest_length);
            if (rest_length != 0 && rest[0] != '#') {
                this->error = "text after the quoted value";
                return false;
            }
            value += 1;
            value_length = static_cast<usz>(close - value);
        } else {
            const char* comment = static_cast<const char*>(memchr(value, '#', value_length));
            if (comment != nullptr) {
                value_length = static_cast<usz>(comment - value);
                trim(&value, &value_length);
            }
        }

        out->section = this->section;
        out->section_length = this->section_length;
        out->key = key;
        out->key_length = key_length;
        out->value = value;
        out->value_length = value_length;
        out->line = this->line;
        return true;
    }
    return false;
}

bool TEXT_ASSET::next_word(const char* value, const usz length, usz* offset, const char** out_word, usz* out_length) {
    usz i = *offset;
    while (i < length && (is_space(value[i]) || value[i] == ',')) {
        ++i;
    }
    if (i >= length) {
        *offset = length;
        return false;
    }
    const usz start = i;
    while (i < length && !is_space(value[i]) && value[i] != ',') {
        ++i;
    }
    *out_word = value + start;
    *out_length = i - start;
    *offset = i;
    return true;
}

bool TEXT_ASSET::parse_numbers(const char* value, const usz length, f64* out, const usz capacity, usz* out_count) {
    usz count = 0;
    usz offset = 0;
    const char* word = nullptr;
    usz word_length = 0;
    while (next_word(value, length, &offset, &word, &word_length)) {
        char buffer[64];
        if (count >= capacity || !copy_span(word, word_length, buffer, sizeof(buffer))) {
            return false;
        }
        char* end = nullptr;
        const f64 number = strtod(buffer, &end);
        if (end == buffer || *end != '\0') {
            return false;
        }
        out[count++] = number;
    }
    *out_count = count;
    return true;
}

bool TEXT_ASSET::copy_span(const char* text, const usz length, char* out, const usz capacity) {
    if (capacity == 0) {
        return false;
    }
    const usz copied = length < capacity - 1 ? length : capacity - 1;
    memcpy(out, text, copied);
    out[copied] = '\0';
    return copied == length;
}

// --- Preludes -----------------------------------------------------------------

bool TEXT_ASSET::find_guid(const char* text, const usz size, AssetGuid* out, u32* out_line) {
    if (out_line != nullptr) {
        *out_line = 0;
    }
    TextAssetCursor cursor(text, size);
    TextAssetEntry entry;
    while (cursor.next(&entry)) {
        if (!entry.in_top_level()) {
            // Top-level keys come first; a guid after a section is not one.
            return false;
        }
        if (entry.key_is("guid")) {
            if (out_line != nullptr) {
                *out_line = entry.line;
            }
            return parse_guid(entry.value, entry.value_length, out);
        }
    }
    return false;
}

AssetView TEXT_ASSET::build_prelude(const u32 type, const AssetGuid& guid, const void* text, const usz size, BaseAllocator* allocator) {
    if (guid.is_null() || extension_of_type(type) == nullptr || size > MAX_FILE_SIZE || (text == nullptr && size != 0)) {
        return AssetView(ASSET_PARSE_BAD_HEADER);
    }
    const usz prelude_size = ASSET_FILE::prelude_size(0, 1);
    u8* buffer = static_cast<u8*>(allocator->allocate(prelude_size, ASSET_FILE::PAYLOAD_ALIGNMENT));
    if (buffer == nullptr) {
        return AssetView(ASSET_PARSE_BAD_HEADER);
    }
    memset(buffer, 0, prelude_size);

    AssetHeader header;
    header.type = type;
    header.flags = ASSET_FLAG::TEXT | ASSET_FLAG::COOKED;
    header.guid = guid;
    header.content_hash = HASH::fnv1a(text, size);
    header.dependency_count = 0;
    header.chunk_count = 1;
    header.file_size = ASSET_FILE::payload_start(0, 1) + size;
    ChunkEntry chunk;
    chunk.tag = CHUNK_TYPE::TEXT;
    chunk.version = VERSION;
    chunk.offset = ASSET_FILE::payload_start(0, 1);
    chunk.size = size;
    memcpy(buffer, &header, sizeof(header));
    memcpy(buffer + sizeof(header), &chunk, sizeof(chunk));

    AssetView view = AssetView::parse(buffer, prelude_size);
    if (!view.is_ok()) {
        allocator->free(buffer);
    }
    return view;
}

AssetView TEXT_ASSET::build_prelude(const char* path, const void* text, const usz size, BaseAllocator* allocator) {
    const u32 type = type_of_path(path);
    if (type == 0) {
        fprintf(stderr, "[asset] error: %s is not a text asset (unknown extension)\n", path != nullptr ? path : "(null)");
        return AssetView(ASSET_PARSE_BAD_HEADER);
    }
    AssetGuid guid;
    u32 line = 0;
    if (!find_guid(static_cast<const char*>(text), size, &guid, &line)) {
        if (line != 0) {
            fprintf(stderr, "[asset] error: %s:%u: malformed guid (32 hex digits expected)\n", path, line);
        } else {
            fprintf(stderr, "[asset] error: %s: no top-level `guid = ...` line\n", path);
        }
        return AssetView(ASSET_PARSE_BAD_HEADER);
    }
    return build_prelude(type, guid, text, size, allocator);
}

bool TEXT_ASSET::read_file(const char* path, BaseAllocator* allocator, u8** out_text, usz* out_size) {
    *out_text = nullptr;
    *out_size = 0;
    File file;
    if (!PLATFORM::file_open(&file, path, FILE_ACCESS_READ)) {
        return false;
    }
    u64 size = 0;
    if (!PLATFORM::file_size(file, &size) || size > MAX_FILE_SIZE) {
        PLATFORM::file_close(&file);
        return false;
    }
    u8* text = static_cast<u8*>(allocator->allocate(size > 0 ? size : 1, ASSET_FILE::PAYLOAD_ALIGNMENT));
    if (text == nullptr) {
        PLATFORM::file_close(&file);
        return false;
    }
    const bool ok = PLATFORM::file_read(file, 0, text, size);
    PLATFORM::file_close(&file);
    if (!ok) {
        allocator->free(text);
        return false;
    }
    *out_text = text;
    *out_size = size;
    return true;
}

AssetView TEXT_ASSET::read_prelude(const char* path, BaseAllocator* allocator) {
    u8* text = nullptr;
    usz size = 0;
    if (!read_file(path, allocator, &text, &size)) {
        return AssetView(ASSET_FILE_ERROR);
    }
    const AssetView view = build_prelude(path, text, size, allocator);
    allocator->free(text);
    return view;
}

bool TEXT_ASSET::matches(const AssetView& view, const void* text, const usz size) {
    if (!view.is_text()) {
        return false;
    }
    return view.header->file_size == ASSET_FILE::payload_start(0, 1) + size && view.header->content_hash == HASH::fnv1a(text, size);
}

AssetView ASSET_FILE::read_prelude_any(const char* path, BaseAllocator* allocator) {
    if (TEXT_ASSET::type_of_path(path) != 0) {
        return TEXT_ASSET::read_prelude(path, allocator);
    }
    return read_prelude(path, allocator);
}
