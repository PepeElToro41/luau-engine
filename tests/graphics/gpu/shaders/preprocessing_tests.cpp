#include "support/test_support.hpp"

#include "engine/gpu/shaders/preprocessing.hpp"

#include <cstring>

TEST_CASE("gpu/shaders/preprocessing: a file without pragmas declares forward with both stages") {
    ShaderDirectives directives;
    CHECK(SHADER_PREPROCESSING::parse_directives("#version 450\nvoid main() {}\n", 0, directives));
    REQUIRE(directives.pass_count == 1);
    CHECK(strcmp(directives.passes[0].name, "forward") == 0);
    CHECK(directives.passes[0].stages == (SHADER_STAGE_MASK_VERTEX | SHADER_STAGE_MASK_FRAGMENT));
    CHECK(directives.passes[0].line == 0);
    CHECK(directives.error[0] == '\0');
}

TEST_CASE("gpu/shaders/preprocessing: pragmas list passes with their stages and lines") {
    const char* text =
        "#version 450\n"
        "#pragma pass forward vertex fragment\n"
        "  #  pragma   pass\tshadow vertex\r\n"
        "#pragma pass post\n"
        "void main() {}\n";
    ShaderDirectives directives;
    CHECK(SHADER_PREPROCESSING::parse_directives(text, 0, directives));
    REQUIRE(directives.pass_count == 3);

    const ShaderPassDirective* forward = directives.find("forward");
    REQUIRE(forward != nullptr);
    CHECK(forward->stages == (SHADER_STAGE_MASK_VERTEX | SHADER_STAGE_MASK_FRAGMENT));
    CHECK(forward->line == 2);

    const ShaderPassDirective* shadow = directives.find("shadow");
    REQUIRE(shadow != nullptr);
    CHECK(shadow->stages == SHADER_STAGE_MASK_VERTEX);
    CHECK(shadow->line == 3);

    SUBCASE("a pass without a stage list gets vertex and fragment") {
        const ShaderPassDirective* post = directives.find("post");
        REQUIRE(post != nullptr);
        CHECK(post->stages == (SHADER_STAGE_MASK_VERTEX | SHADER_STAGE_MASK_FRAGMENT));
        CHECK(post->line == 4);
    }
    SUBCASE("find returns nullptr for an unknown pass") {
        CHECK(directives.find("depth") == nullptr);
    }
}

TEST_CASE("gpu/shaders/preprocessing: size limits the scan and a missing trailing newline is fine") {
    const char* text = "#pragma pass a\n#pragma pass b";
    ShaderDirectives directives;
    CHECK(SHADER_PREPROCESSING::parse_directives(text, 15, directives));
    CHECK(directives.pass_count == 1);
    CHECK(SHADER_PREPROCESSING::parse_directives(text, 0, directives));
    CHECK(directives.pass_count == 2);
}

TEST_CASE("gpu/shaders/preprocessing: other pragmas and similar words are not passes") {
    const char* text = "#pragma once\n#pragma passthrough x\n#define pass 1\n// #pragma pass comment\n";
    ShaderDirectives directives;
    CHECK(SHADER_PREPROCESSING::parse_directives(text, 0, directives));
    CHECK(directives.pass_count == 1);
    CHECK(strcmp(directives.passes[0].name, "forward") == 0);
}

TEST_CASE("gpu/shaders/preprocessing: parse errors name the line and leave no passes") {
    ShaderDirectives directives;
    SUBCASE("duplicate pass") {
        CHECK_FALSE(SHADER_PREPROCESSING::parse_directives("#pragma pass forward\n#pragma pass forward\n", 0, directives));
        CHECK(strstr(directives.error, "line 2") != nullptr);
        CHECK(strstr(directives.error, "duplicate") != nullptr);
    }
    SUBCASE("unknown stage") {
        CHECK_FALSE(SHADER_PREPROCESSING::parse_directives("#pragma pass forward vertex geometry\n", 0, directives));
        CHECK(strstr(directives.error, "line 1") != nullptr);
        CHECK(strstr(directives.error, "unknown stage") != nullptr);
    }
    SUBCASE("punctuation after the stages") {
        CHECK_FALSE(SHADER_PREPROCESSING::parse_directives("#pragma pass forward vertex, fragment\n", 0, directives));
    }
    SUBCASE("missing name") {
        CHECK_FALSE(SHADER_PREPROCESSING::parse_directives("#pragma pass\n", 0, directives));
        CHECK(strstr(directives.error, "name") != nullptr);
    }
    SUBCASE("name too long") {
        char text[128] = "#pragma pass ";
        for (u32 i = 0; i < SHADER_PREPROCESSING::PASS_NAME_MAX; ++i) {
            strcat(text, "x");
        }
        strcat(text, "\n");
        CHECK_FALSE(SHADER_PREPROCESSING::parse_directives(text, 0, directives));
        CHECK(strstr(directives.error, "too long") != nullptr);
    }
    SUBCASE("too many passes") {
        char text[512] = "";
        for (u32 i = 0; i <= SHADER_PREPROCESSING::MAX_PASSES; ++i) {
            char line[32];
            snprintf(line, sizeof(line), "#pragma pass p%u\n", i);
            strcat(text, line);
        }
        CHECK_FALSE(SHADER_PREPROCESSING::parse_directives(text, 0, directives));
        CHECK(strstr(directives.error, "too many") != nullptr);
    }
    CHECK(directives.pass_count == 0);
}

TEST_CASE("gpu/shaders/preprocessing: strip blanks pragma lines and keeps every newline") {
    char text[] = "#version 450\n#pragma pass forward vertex fragment\r\n#pragma once\nvoid main() {}\n";
    const usz length = strlen(text);
    u32 newlines_before = 0;
    for (usz i = 0; i < length; ++i) {
        newlines_before += text[i] == '\n';
    }

    SHADER_PREPROCESSING::strip_directives(text, 0);

    CHECK(strlen(text) == length);
    u32 newlines_after = 0;
    for (usz i = 0; i < length; ++i) {
        newlines_after += text[i] == '\n';
    }
    CHECK(newlines_after == newlines_before);
    CHECK(strstr(text, "pragma pass") == nullptr);
    CHECK(strstr(text, "#pragma once") != nullptr);
    CHECK(strstr(text, "#version 450") != nullptr);
    CHECK(strstr(text, "\r\n") != nullptr);
}

TEST_CASE("gpu/shaders/preprocessing: stage and pass defines name the compile variant") {
    CHECK(strcmp(SHADER_PREPROCESSING::stage_define(SHADER_STAGE_VERTEX), "STAGE_VERTEX") == 0);
    CHECK(strcmp(SHADER_PREPROCESSING::stage_define(SHADER_STAGE_FRAGMENT), "STAGE_FRAGMENT") == 0);
    CHECK(strcmp(SHADER_PREPROCESSING::stage_define(SHADER_STAGE_COMPUTE), "STAGE_COMPUTE") == 0);

    CHECK(strcmp(SHADER_PREPROCESSING::stage_entry_point(SHADER_STAGE_VERTEX), "vertex") == 0);
    CHECK(strcmp(SHADER_PREPROCESSING::stage_entry_point(SHADER_STAGE_FRAGMENT), "fragment") == 0);
    CHECK(strcmp(SHADER_PREPROCESSING::stage_entry_point(SHADER_STAGE_COMPUTE), "compute") == 0);

    char define[SHADER_PREPROCESSING::PASS_DEFINE_MAX];
    SHADER_PREPROCESSING::pass_define("forward", define, sizeof(define));
    CHECK(strcmp(define, "PASS_FORWARD") == 0);
    SHADER_PREPROCESSING::pass_define("shadow_cascade1", define, sizeof(define));
    CHECK(strcmp(define, "PASS_SHADOW_CASCADE1") == 0);

    SUBCASE("a define that does not fit is truncated, never overrun") {
        char small[8];
        SHADER_PREPROCESSING::pass_define("forward", small, sizeof(small));
        CHECK(strcmp(small, "PASS_FO") == 0);
    }
}

TEST_CASE("gpu/shaders/preprocessing: enable_includes inserts the extension after #version only") {
    SUBCASE("after a #version line, keeping the numbering of the original file") {
        const char* original = "#version 450\n#pragma pass forward\nvoid main() {}\n";
        char text[256];
        strcpy(text, original);
        const usz size = SHADER_PREPROCESSING::enable_includes(text, strlen(original));
        CHECK(size == strlen(original) + SHADER_PREPROCESSING::INCLUDE_EXTENSION_LENGTH);
        CHECK(strlen(text) == size);
        CHECK(strncmp(text, "#version 450\n#extension GL_GOOGLE_include_directive : enable\n#line 2\n#pragma pass forward\n", 0) == 0);
        CHECK(strstr(text, "#line 2\n#pragma pass forward\nvoid main() {}\n") != nullptr);
        CHECK(strstr(text, "#version 450\n#extension") == text);
    }
    SUBCASE("a file without #version is left alone") {
        char text[64] = "void main() {}\n";
        const usz before = strlen(text);
        CHECK(SHADER_PREPROCESSING::enable_includes(text, before) == before);
        CHECK(strcmp(text, "void main() {}\n") == 0);
    }
    SUBCASE("a lone #version line with no newline is left alone") {
        char text[64] = "#version 450";
        CHECK(SHADER_PREPROCESSING::enable_includes(text, strlen(text)) == strlen("#version 450"));
        CHECK(strcmp(text, "#version 450") == 0);
    }
}

TEST_CASE("gpu/shaders/preprocessing: resolve_include fails cleanly when nothing opens") {
    const char* roots[] = {"/nonexistent/luau-engine-include-root"};
    char out[256] = "garbage";
    CHECK_FALSE(SHADER_PREPROCESSING::resolve_include("missing.glsl", true, "/nonexistent/a/b.glsl", roots, 1, out, sizeof(out)));
    CHECK(out[0] == '\0');
    CHECK_FALSE(SHADER_PREPROCESSING::resolve_include("", false, nullptr, roots, 1, out, sizeof(out)));
    CHECK_FALSE(SHADER_PREPROCESSING::resolve_include("x.glsl", true, nullptr, nullptr, 0, out, sizeof(out)));
}
