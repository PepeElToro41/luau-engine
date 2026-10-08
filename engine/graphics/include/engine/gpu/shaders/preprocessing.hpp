#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/shaders/shader.hpp"

// Everything the engine does to a shader's text before the compiler sees
// it. Pure text processing plus path lookups, no GPU, so it is unit-tested
// (tests/graphics/gpu/shaders/preprocessing_tests.cpp).
//
// 1. Pass directives: the engine's one addition to the shader language. A file
//    declares which render passes it can draw in, and which stages each
//    needs, with pragma lines at the top of the file:
//
//        #pragma pass forward vertex fragment
//        #pragma pass shadow vertex
//
//    A pass with no stage list gets `vertex fragment`. A file with no
//    pragma at all declares one pass, "forward", with both stages. The
//    compilers only warn about pragmas they do not know, but
//    strip_directives() blanks them anyway so nothing downstream sees them.
//
// 2. Defines and entry points: the program loader compiles the file once
//    per (pass, stage) with STAGE_VERTEX / STAGE_FRAGMENT / STAGE_COMPUTE
//    and PASS_<NAME> defined; stage_define() and pass_define() name them.
//    In Slang each stage is the [shader("...")] function stage_entry_point()
//    names (`vertex`, `fragment`, `compute`), all in one file; GLSL keeps
//    one `main` per #ifdef STAGE_* section.
//
// 3. Includes: enable_includes() turns `#include` on in a GLSL source
//    whatever shaderc was built with; resolve_include() finds the file an
//    `#include` names, relative to the including file first, then in the
//    include roots. The compiler's includer calls it.

// A preprocessor definition, `name` or `name=value`.
struct ShaderDefine {
    const char* name = nullptr;
    // nullptr defines `name` with no value.
    const char* value = nullptr;
};

namespace SHADER_PREPROCESSING {

constexpr u32 MAX_PASSES = 8;
constexpr u32 PASS_NAME_MAX = 32;
constexpr u32 ERROR_MAX = 128;
// Room for "PASS_" plus a pass name.
constexpr u32 PASS_DEFINE_MAX = PASS_NAME_MAX + 8;

} // namespace SHADER_PREPROCESSING

struct ShaderPassDirective {
    char name[SHADER_PREPROCESSING::PASS_NAME_MAX] = {};
    // ShaderStageMask bits.
    u32 stages = 0;
    // 1-based line of the pragma, 0 for the implicit default pass.
    u32 line = 0;
};

struct ShaderDirectives {
    ShaderPassDirective passes[SHADER_PREPROCESSING::MAX_PASSES] = {};
    u32 pass_count = 0;
    // "line N: message" when parse_directives() fails, empty otherwise.
    char error[SHADER_PREPROCESSING::ERROR_MAX] = {};

    const ShaderPassDirective* find(const char* name) const;
};

namespace SHADER_PREPROCESSING {

// --- Pass directives ----------------------------------------------------------

// Scans `size` bytes of `text` (0 means null-terminated) for `#pragma pass`
// lines and fills `out`. False with `out.error` set on a duplicate pass, an
// unknown stage word, a name that is empty or too long, or more than
// MAX_PASSES passes; `out` then holds nothing.
bool parse_directives(const char* text, usz size, ShaderDirectives& out);

// Overwrites every `#pragma pass` line in `text` with spaces, keeping the
// newlines so line numbers in compiler messages still match the file.
void strip_directives(char* text, usz size);

// --- Defines ----------------------------------------------------------------------

// "STAGE_VERTEX", "STAGE_FRAGMENT" or "STAGE_COMPUTE"; static storage.
const char* stage_define(ShaderStage stage);

// Writes "PASS_<NAME>" for `pass_name`, upper-cased, into `out` (at most
// `out_size` bytes, PASS_DEFINE_MAX always fits a valid pass name).
void pass_define(const char* pass_name, char* out, usz out_size);

// The Slang entry point a stage compiles: "vertex", "fragment" or
// "compute"; static storage.
const char* stage_entry_point(ShaderStage stage);

// --- Includes ---------------------------------------------------------------------

// Inserted after a `#version` line so `#include` works regardless of how
// shaderc was built; the `#line` keeps the numbering of the original file.
inline constexpr char INCLUDE_EXTENSION[] = "#extension GL_GOOGLE_include_directive : enable\n#line 2\n";
constexpr usz INCLUDE_EXTENSION_LENGTH = sizeof(INCLUDE_EXTENSION) - 1;

// Inserts INCLUDE_EXTENSION after the first line of `text` when that line is
// a `#version` directive, and returns the new size; otherwise leaves the
// text alone and returns `size`. `text` must be null-terminated with room
// for INCLUDE_EXTENSION_LENGTH more bytes.
usz enable_includes(char* text, usz size);

// Finds the file an `#include` names. `relative` is the `"x"` form, which
// tries the directory of `requesting` (the including file's path) first;
// both forms then try each of the `include_dir_count` roots. The first
// candidate that opens goes to `out`. False, with `out` empty, when none
// does or the path would not fit.
bool resolve_include(const char* requested, bool relative, const char* requesting, const char* const* include_dirs, usz include_dir_count, char* out, usz out_size);

} // namespace SHADER_PREPROCESSING
