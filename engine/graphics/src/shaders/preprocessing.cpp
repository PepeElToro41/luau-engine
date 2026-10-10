#include "engine/shaders/preprocessing.hpp"

#include "engine/platform/file.hpp"

#include <cstdio>
#include <cstring>

// --- Line scanning -------------------------------------------------------------

static bool is_space(const char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static bool is_word_char(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Advances past spaces. `end` is one past the last byte of the line.
static const char* skip_spaces(const char* cursor, const char* end) {
    while (cursor < end && is_space(*cursor)) {
        ++cursor;
    }
    return cursor;
}

// True if the line [begin, end) is `#pragma pass ...`; `out_args` then points
// at whatever follows the word `pass`.
static bool is_pass_pragma(const char* begin, const char* end, const char** out_args) {
    const char* cursor = skip_spaces(begin, end);
    if (cursor >= end || *cursor != '#') {
        return false;
    }
    cursor = skip_spaces(cursor + 1, end);
    static constexpr const char* PRAGMA = "pragma";
    static constexpr const char* PASS = "pass";
    const usz pragma_length = strlen(PRAGMA);
    const usz pass_length = strlen(PASS);
    if (static_cast<usz>(end - cursor) < pragma_length || strncmp(cursor, PRAGMA, pragma_length) != 0) {
        return false;
    }
    cursor += pragma_length;
    if (cursor >= end || !is_space(*cursor)) {
        return false;
    }
    cursor = skip_spaces(cursor, end);
    if (static_cast<usz>(end - cursor) < pass_length || strncmp(cursor, PASS, pass_length) != 0) {
        return false;
    }
    cursor += pass_length;
    if (cursor < end && is_word_char(*cursor)) {
        // `#pragma passthrough` or the like.
        return false;
    }
    *out_args = cursor;
    return true;
}

// Reads one word into `out` (at most `max` bytes including the terminator).
// Returns the cursor after it; `out` is empty if there was no word. False if
// the word did not fit.
static bool read_word(const char*& cursor, const char* end, char* out, const usz max) {
    cursor = skip_spaces(cursor, end);
    usz length = 0;
    while (cursor < end && is_word_char(*cursor)) {
        if (length + 1 >= max) {
            return false;
        }
        out[length++] = *cursor++;
    }
    out[length] = '\0';
    return true;
}

static bool set_error(ShaderDirectives& out, const u32 line, const char* message) {
    snprintf(out.error, sizeof(out.error), "line %u: %s", line, message);
    out.pass_count = 0;
    return false;
}

// --- ShaderDirectives ----------------------------------------------------------

const ShaderPassDirective* ShaderDirectives::find(const char* name) const {
    for (u32 i = 0; i < this->pass_count; ++i) {
        if (strcmp(this->passes[i].name, name) == 0) {
            return &this->passes[i];
        }
    }
    return nullptr;
}

// --- Pass directives ---------------------------------------------------------------

bool SHADER_PREPROCESSING::parse_directives(const char* text, usz size, ShaderDirectives& out) {
    out = ShaderDirectives{};
    if (text == nullptr) {
        return set_error(out, 0, "no text");
    }
    if (size == 0) {
        size = strlen(text);
    }

    const char* end = text + size;
    const char* line_begin = text;
    u32 line_number = 1;
    while (line_begin < end) {
        const char* line_end = static_cast<const char*>(memchr(line_begin, '\n', static_cast<usz>(end - line_begin)));
        if (line_end == nullptr) {
            line_end = end;
        }

        const char* args = nullptr;
        if (is_pass_pragma(line_begin, line_end, &args)) {
            if (out.pass_count >= MAX_PASSES) {
                return set_error(out, line_number, "too many passes");
            }
            ShaderPassDirective& pass = out.passes[out.pass_count];
            if (!read_word(args, line_end, pass.name, sizeof(pass.name))) {
                return set_error(out, line_number, "pass name too long");
            }
            if (pass.name[0] == '\0') {
                return set_error(out, line_number, "#pragma pass needs a name");
            }
            if (out.find(pass.name) != nullptr) {
                return set_error(out, line_number, "duplicate pass");
            }
            pass.line = line_number;
            pass.stages = 0;

            char word[16];
            for (;;) {
                if (!read_word(args, line_end, word, sizeof(word))) {
                    return set_error(out, line_number, "unknown stage");
                }
                if (word[0] == '\0') {
                    break;
                }
                if (strcmp(word, "vertex") == 0) {
                    pass.stages |= SHADER_STAGE_MASK_VERTEX;
                } else if (strcmp(word, "fragment") == 0) {
                    pass.stages |= SHADER_STAGE_MASK_FRAGMENT;
                } else if (strcmp(word, "compute") == 0) {
                    pass.stages |= SHADER_STAGE_MASK_COMPUTE;
                } else {
                    return set_error(out, line_number, "unknown stage");
                }
            }
            args = skip_spaces(args, line_end);
            if (args < line_end) {
                // Something that is not a word, e.g. punctuation.
                return set_error(out, line_number, "unknown stage");
            }
            if (pass.stages == 0) {
                pass.stages = SHADER_STAGE_MASK_VERTEX | SHADER_STAGE_MASK_FRAGMENT;
            }
            out.pass_count += 1;
        }

        line_begin = line_end + 1;
        line_number += 1;
    }

    if (out.pass_count == 0) {
        ShaderPassDirective& pass = out.passes[0];
        strcpy(pass.name, "forward");
        pass.stages = SHADER_STAGE_MASK_VERTEX | SHADER_STAGE_MASK_FRAGMENT;
        pass.line = 0;
        out.pass_count = 1;
    }
    return true;
}

void SHADER_PREPROCESSING::strip_directives(char* text, usz size) {
    if (text == nullptr) {
        return;
    }
    if (size == 0) {
        size = strlen(text);
    }
    char* end = text + size;
    char* line_begin = text;
    while (line_begin < end) {
        char* line_end = static_cast<char*>(memchr(line_begin, '\n', static_cast<usz>(end - line_begin)));
        if (line_end == nullptr) {
            line_end = end;
        }
        const char* args = nullptr;
        if (is_pass_pragma(line_begin, line_end, &args)) {
            for (char* c = line_begin; c < line_end; ++c) {
                if (*c != '\r') {
                    *c = ' ';
                }
            }
        }
        line_begin = line_end + 1;
    }
}

// --- Defines --------------------------------------------------------------------------

const char* SHADER_PREPROCESSING::stage_define(const ShaderStage stage) {
    switch (stage) {
    case SHADER_STAGE_VERTEX:
        return "STAGE_VERTEX";
    case SHADER_STAGE_FRAGMENT:
        return "STAGE_FRAGMENT";
    case SHADER_STAGE_COMPUTE:
        return "STAGE_COMPUTE";
    }
    return "STAGE_UNKNOWN";
}

const char* SHADER_PREPROCESSING::stage_entry_point(const ShaderStage stage) {
    switch (stage) {
    case SHADER_STAGE_VERTEX:
        return "vertex";
    case SHADER_STAGE_FRAGMENT:
        return "fragment";
    case SHADER_STAGE_COMPUTE:
        return "compute";
    }
    return "main";
}

void SHADER_PREPROCESSING::pass_define(const char* pass_name, char* out, const usz out_size) {
    if (out_size == 0) {
        return;
    }
    snprintf(out, out_size, "PASS_%s", pass_name != nullptr ? pass_name : "");
    for (char* c = out; *c != '\0'; ++c) {
        if (*c >= 'a' && *c <= 'z') {
            *c = static_cast<char>(*c - 'a' + 'A');
        }
    }
}

// --- Includes -------------------------------------------------------------------------

usz SHADER_PREPROCESSING::enable_includes(char* text, const usz size) {
    if (text == nullptr) {
        return 0;
    }
    // Only after a leading #version line: the extension must follow it.
    usz first_line = 0;
    while (first_line < size && text[first_line] != '\n') {
        ++first_line;
    }
    if (first_line >= size || strncmp(text, "#version", 8) != 0) {
        return size;
    }
    first_line += 1;
    memmove(text + first_line + INCLUDE_EXTENSION_LENGTH, text + first_line, size - first_line + 1);
    memcpy(text + first_line, INCLUDE_EXTENSION, INCLUDE_EXTENSION_LENGTH);
    return size + INCLUDE_EXTENSION_LENGTH;
}

// The directory part of `path` into `out`, without the trailing slash.
// False, with `out` empty, when there is none or it does not fit.
static bool directory_of(const char* path, char* out, const usz out_size) {
    out[0] = '\0';
    if (path == nullptr) {
        return false;
    }
    const char* last_slash = nullptr;
    for (const char* c = path; *c != '\0'; ++c) {
        if (*c == '/' || *c == '\\') {
            last_slash = c;
        }
    }
    if (last_slash == nullptr) {
        return false;
    }
    const usz length = static_cast<usz>(last_slash - path);
    if (length + 1 >= out_size) {
        return false;
    }
    memcpy(out, path, length);
    out[length] = '\0';
    return true;
}

// Whether `directory/requested` can be opened; the path goes to `out`.
static bool try_candidate(const char* directory, const char* requested, char* out, const usz out_size) {
    const int n = snprintf(out, out_size, "%s/%s", directory, requested);
    if (n < 0 || static_cast<usz>(n) >= out_size) {
        return false;
    }
    File file;
    if (!PLATFORM::file_open(&file, out, FILE_ACCESS_READ)) {
        return false;
    }
    PLATFORM::file_close(&file);
    return true;
}

bool SHADER_PREPROCESSING::resolve_include(const char* requested, const bool relative, const char* requesting, const char* const* include_dirs, const usz include_dir_count, char* out, const usz out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (requested == nullptr || requested[0] == '\0') {
        return false;
    }
    // "x": the including file's directory first.
    if (relative) {
        char directory[1024];
        if (directory_of(requesting, directory, sizeof(directory)) && try_candidate(directory, requested, out, out_size)) {
            return true;
        }
    }
    // Then the roots, for both forms.
    for (usz i = 0; i < include_dir_count; ++i) {
        if (include_dirs[i] != nullptr && try_candidate(include_dirs[i], requested, out, out_size)) {
            return true;
        }
    }
    out[0] = '\0';
    return false;
}
