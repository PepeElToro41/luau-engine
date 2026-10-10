#include "support/test_support.hpp"

#include "engine/asset/asset_reader.hpp"
#include "engine/asset/asset_types/material_asset.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

const AssetGuid MATERIAL_GUID = {0x179a52dd06b33f96ull, 0xe26f6a98c55cbe22ull};
constexpr const char* MATERIAL_GUID_TEXT = "e26f6a98c55cbe22179a52dd06b33f96";
const AssetGuid ALBEDO_GUID = {0x1122334455667788ull, 0x99aabbccddeeff00ull};
constexpr const char* ALBEDO_GUID_TEXT = "99aabbccddeeff001122334455667788";

const std::string SAMPLE = "# A material\n"
                           "guid = " + std::string(MATERIAL_GUID_TEXT) + "\n"
                           "shader = textured\n"
                           "\n"
                           "[params]\n"
                           "tint = 1 0.5 0.15 1\n"
                           "tiles = 3\n"
                           "\n"
                           "[textures]\n"
                           "albedo = " + std::string(ALBEDO_GUID_TEXT) + "\n"
                           "detail = none\n"
                           "\n"
                           "[samplers]\n"
                           "albedo_sampler = nearest clamp anisotropy=8\n";

std::string written(const MaterialAsset& material) {
    DynamicArray<char> out;
    MATERIAL_ASSET::write(material, out);
    std::string text(out.data, out.count);
    out.free();
    return text;
}

MaterialParseError parse(const std::string& text, MaterialAsset& out, u32* line = nullptr) {
    return MATERIAL_ASSET::parse(text.data(), text.size(), out, line);
}

} // namespace

TEST_CASE("asset/material_asset: parse reads the shader, params, textures and samplers") {
    MaterialAsset material;
    u32 line = 99;
    REQUIRE(parse(SAMPLE, material, &line) == MATERIAL_PARSE_OK);
    CHECK(line == 0);
    CHECK(material.guid == MATERIAL_GUID);
    CHECK(std::string(material.shader) == "textured");

    REQUIRE(material.param_count == 2);
    const MaterialParam* tint = material.find_param("tint");
    REQUIRE(tint != nullptr);
    REQUIRE(tint->count == 4);
    CHECK(tint->values[0] == 1.0);
    CHECK(tint->values[1] == 0.5);
    CHECK(tint->values[2] == 0.15);
    CHECK(tint->values[3] == 1.0);
    const MaterialParam* tiles = material.find_param("tiles");
    REQUIRE(tiles != nullptr);
    CHECK(tiles->count == 1);
    CHECK(tiles->values[0] == 3.0);
    CHECK(material.find_param("missing") == nullptr);

    REQUIRE(material.texture_count == 2);
    const MaterialTextureRef* albedo = material.find_texture("albedo");
    REQUIRE(albedo != nullptr);
    CHECK(albedo->texture == ALBEDO_GUID);
    const MaterialTextureRef* detail = material.find_texture("detail");
    REQUIRE(detail != nullptr);
    CHECK(detail->texture.is_null());

    REQUIRE(material.sampler_count == 1);
    const MaterialSamplerRef* sampler = material.find_sampler("albedo_sampler");
    REQUIRE(sampler != nullptr);
    CHECK(sampler->sampler.min_filter == MATERIAL_FILTER_NEAREST);
    CHECK(sampler->sampler.mag_filter == MATERIAL_FILTER_NEAREST);
    CHECK(sampler->sampler.mip_filter == MATERIAL_FILTER_NEAREST);
    CHECK(sampler->sampler.address_u == MATERIAL_ADDRESS_CLAMP);
    CHECK(sampler->sampler.address_w == MATERIAL_ADDRESS_CLAMP);
    CHECK(sampler->sampler.max_anisotropy == 8.0f);
}

TEST_CASE("asset/material_asset: parse reports what is wrong and where") {
    MaterialAsset material;
    u32 line = 0;
    SUBCASE("missing guid") {
        CHECK(parse("shader = unlit\n", material, &line) == MATERIAL_PARSE_MISSING_GUID);
        CHECK(line == 0);
    }
    SUBCASE("missing shader") {
        CHECK(parse("guid = " + std::string(MATERIAL_GUID_TEXT) + "\n", material, &line) == MATERIAL_PARSE_MISSING_SHADER);
    }
    SUBCASE("a bad guid") {
        CHECK(parse("guid = nope\nshader = unlit\n", material, &line) == MATERIAL_PARSE_BAD_GUID);
        CHECK(line == 1);
    }
    SUBCASE("a bad texture guid") {
        CHECK(parse("guid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = unlit\n[textures]\nalbedo = 12\n", material, &line) == MATERIAL_PARSE_BAD_GUID);
        CHECK(line == 4);
    }
    SUBCASE("a param that is not numbers") {
        CHECK(parse("guid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = unlit\n[params]\ncolor = red\n", material, &line) == MATERIAL_PARSE_BAD_VALUE);
        CHECK(line == 4);
    }
    SUBCASE("an empty param") {
        CHECK(parse("guid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = unlit\n[params]\ncolor =\n", material, &line) == MATERIAL_PARSE_BAD_VALUE);
    }
    SUBCASE("a sampler word that is not one") {
        CHECK(parse("guid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = unlit\n[samplers]\ns = bilinear\n", material, &line) == MATERIAL_PARSE_BAD_VALUE);
        CHECK(line == 4);
    }
    SUBCASE("an unknown section or top-level key") {
        CHECK(parse("guid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = unlit\n[lights]\nx = 1\n", material, &line) == MATERIAL_PARSE_UNKNOWN_KEY);
        CHECK(line == 4);
        CHECK(parse("guid = " + std::string(MATERIAL_GUID_TEXT) + "\nname = x\nshader = unlit\n", material, &line) == MATERIAL_PARSE_UNKNOWN_KEY);
        CHECK(line == 2);
    }
    SUBCASE("a name given twice") {
        CHECK(parse("guid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = unlit\n[params]\na = 1\na = 2\n", material, &line) == MATERIAL_PARSE_DUPLICATE);
        CHECK(line == 5);
        CHECK(parse("guid = " + std::string(MATERIAL_GUID_TEXT) + "\nguid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = unlit\n", material, &line) ==
              MATERIAL_PARSE_DUPLICATE);
    }
    SUBCASE("a malformed line") {
        CHECK(parse("guid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = unlit\n[params]\nbroken\n", material, &line) == MATERIAL_PARSE_BAD_SYNTAX);
        CHECK(line == 4);
    }
    SUBCASE("too many params") {
        std::string text = "guid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = unlit\n[params]\n";
        for (u32 i = 0; i <= MATERIAL_ASSET::MAX_PARAMS; ++i) {
            text += "p" + std::to_string(i) + " = 1\n";
        }
        CHECK(parse(text, material, &line) == MATERIAL_PARSE_TOO_MANY);
        CHECK(line == 4 + MATERIAL_ASSET::MAX_PARAMS);
    }
    SUBCASE("every error has a name") {
        CHECK(std::strlen(MATERIAL_ASSET::parse_error_name(MATERIAL_PARSE_OK)) > 0);
        CHECK(std::strlen(MATERIAL_ASSET::parse_error_name(MATERIAL_PARSE_TOO_MANY)) > 0);
    }
}

TEST_CASE("asset/material_asset: parse from a text asset checks the prelude") {
    MaterialAsset material;
    AssetView view = TEXT_ASSET::build_prelude(ASSET_TYPE::MATERIAL, MATERIAL_GUID, SAMPLE.data(), SAMPLE.size(), MEMORY::heap_allocator());
    REQUIRE(view.is_ok());
    CHECK(MATERIAL_ASSET::parse(view, SAMPLE.data(), SAMPLE.size(), material) == MATERIAL_PARSE_OK);
    CHECK(material.param_count == 2);

    SUBCASE("a text naming another guid than the prelude is refused") {
        const std::string other = "guid = " + std::string(ALBEDO_GUID_TEXT) + "\nshader = unlit\n";
        CHECK(MATERIAL_ASSET::parse(view, other.data(), other.size(), material) == MATERIAL_PARSE_BAD_GUID);
    }
    SUBCASE("a prelude of another type is refused") {
        AssetView empty;
        CHECK(MATERIAL_ASSET::parse(empty, SAMPLE.data(), SAMPLE.size(), material) == MATERIAL_PARSE_BAD_SYNTAX);
    }
    ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());
}

TEST_CASE("asset/material_asset: write produces text that parses back to the same material") {
    MaterialAsset material;
    REQUIRE(parse(SAMPLE, material) == MATERIAL_PARSE_OK);
    const std::string text = written(material);

    MaterialAsset again;
    REQUIRE(parse(text, again) == MATERIAL_PARSE_OK);
    CHECK(again.guid == material.guid);
    CHECK(std::string(again.shader) == "textured");
    REQUIRE(again.param_count == 2);
    for (u32 i = 0; i < 2; ++i) {
        CHECK(std::string(again.params[i].name) == material.params[i].name);
        REQUIRE(again.params[i].count == material.params[i].count);
        for (u32 v = 0; v < again.params[i].count; ++v) {
            CHECK(static_cast<f32>(again.params[i].values[v]) == static_cast<f32>(material.params[i].values[v]));
        }
    }
    REQUIRE(again.texture_count == 2);
    CHECK(again.find_texture("albedo")->texture == ALBEDO_GUID);
    CHECK(again.find_texture("detail")->texture.is_null());
    REQUIRE(again.sampler_count == 1);
    CHECK(again.find_sampler("albedo_sampler")->sampler == material.find_sampler("albedo_sampler")->sampler);

    SUBCASE("the layout is canonical and numbers are short") {
        CHECK(text.find("guid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = textured\n") == 0);
        CHECK(text.find("tint = 1 0.5 0.15 1\n") != std::string::npos);
        CHECK(text.find("tiles = 3\n") != std::string::npos);
        CHECK(text.find("detail = none\n") != std::string::npos);
        CHECK(text.find("albedo_sampler = nearest clamp anisotropy=8\n") != std::string::npos);
        // Writing the parsed text again changes nothing.
        CHECK(written(again) == text);
    }
    SUBCASE("empty sections are omitted") {
        MaterialAsset bare;
        bare.guid = MATERIAL_GUID;
        std::strcpy(bare.shader, "unlit");
        const std::string minimal = written(bare);
        CHECK(minimal == "guid = " + std::string(MATERIAL_GUID_TEXT) + "\nshader = unlit\n");
    }
    SUBCASE("float values that are not short decimals still round-trip exactly") {
        MaterialAsset odd;
        odd.guid = MATERIAL_GUID;
        std::strcpy(odd.shader, "unlit");
        const f64 values[3] = {1.0 / 3.0, 1e-7, 123456.789};
        REQUIRE(odd.set_param("v", values, 3));
        MaterialAsset back;
        REQUIRE(parse(written(odd), back) == MATERIAL_PARSE_OK);
        for (u32 i = 0; i < 3; ++i) {
            CHECK(static_cast<f32>(back.params[0].values[i]) == static_cast<f32>(values[i]));
        }
    }
}

TEST_CASE("asset/material_asset: samplers read and write their words") {
    MaterialSamplerDesc sampler;
    SUBCASE("defaults") {
        REQUIRE(MATERIAL_ASSET::parse_sampler("", 0, &sampler));
        CHECK(sampler == MaterialSamplerDesc());
        char words[128];
        MATERIAL_ASSET::format_sampler(sampler, words, sizeof(words));
        CHECK(std::string(words) == "linear repeat");
    }
    SUBCASE("per-axis overrides after the bare words") {
        const char text[] = "linear mirror mag=nearest v=clamp anisotropy=2.5";
        REQUIRE(MATERIAL_ASSET::parse_sampler(text, std::strlen(text), &sampler));
        CHECK(sampler.min_filter == MATERIAL_FILTER_LINEAR);
        CHECK(sampler.mag_filter == MATERIAL_FILTER_NEAREST);
        CHECK(sampler.mip_filter == MATERIAL_FILTER_LINEAR);
        CHECK(sampler.address_u == MATERIAL_ADDRESS_MIRROR);
        CHECK(sampler.address_v == MATERIAL_ADDRESS_CLAMP);
        CHECK(sampler.address_w == MATERIAL_ADDRESS_MIRROR);
        CHECK(sampler.max_anisotropy == 2.5f);
        char words[128];
        MATERIAL_ASSET::format_sampler(sampler, words, sizeof(words));
        CHECK(std::string(words) == "min=linear mag=nearest mip=linear u=mirror v=clamp w=mirror anisotropy=2.5");
        MaterialSamplerDesc back;
        REQUIRE(MATERIAL_ASSET::parse_sampler(words, std::strlen(words), &back));
        CHECK(back == sampler);
    }
    SUBCASE("unknown words and keys are refused") {
        const char* bad[] = {"trilinear", "min=repeat", "u=linear", "anisotropy=-1", "anisotropy=x", "size=4"};
        for (const char* text : bad) {
            CHECK_FALSE(MATERIAL_ASSET::parse_sampler(text, std::strlen(text), &sampler));
        }
    }
}

TEST_CASE("asset/material_asset: setters replace by name and respect the tables") {
    MaterialAsset material;
    CHECK(material.set_param("a", 1.0));
    CHECK(material.set_param("a", 2.0));
    CHECK(material.param_count == 1);
    CHECK(material.find_param("a")->values[0] == 2.0);
    const f64 too_many[MATERIAL_ASSET::MAX_VALUES + 1] = {};
    CHECK_FALSE(material.set_param("b", too_many, MATERIAL_ASSET::MAX_VALUES + 1));
    CHECK_FALSE(material.set_param("", 1.0));
    CHECK_FALSE(material.set_param(nullptr, 1.0));
    std::string long_name(MATERIAL_ASSET::NAME_CAPACITY, 'n');
    CHECK_FALSE(material.set_param(long_name.c_str(), 1.0));
    for (u32 i = 1; i < MATERIAL_ASSET::MAX_PARAMS; ++i) {
        CHECK(material.set_param(("p" + std::to_string(i)).c_str(), 1.0));
    }
    CHECK(material.param_count == MATERIAL_ASSET::MAX_PARAMS);
    CHECK_FALSE(material.set_param("one_more", 1.0));
    CHECK(material.set_param("a", 3.0)); // replacing still works when full

    CHECK(material.set_texture("albedo", ALBEDO_GUID));
    CHECK(material.set_texture("albedo", AssetGuid{}));
    CHECK(material.texture_count == 1);
    CHECK(material.find_texture("albedo")->texture.is_null());

    MaterialSamplerDesc nearest;
    nearest.min_filter = MATERIAL_FILTER_NEAREST;
    CHECK(material.set_sampler("s", nearest));
    CHECK(material.find_sampler("s")->sampler == nearest);
    material.clear();
    CHECK(material.param_count == 0);
    CHECK(material.texture_count == 0);
    CHECK(material.sampler_count == 0);
}

TEST_CASE("asset/material_asset: write_file writes a file read_prelude scans") {
    const std::string path = (std::filesystem::temp_directory_path() / "luau_engine_material_write.material").string();
    MaterialAsset material;
    REQUIRE(parse(SAMPLE, material) == MATERIAL_PARSE_OK);
    REQUIRE(MATERIAL_ASSET::write_file(material, path.c_str()));

    AssetView view = TEXT_ASSET::read_prelude(path.c_str(), MEMORY::heap_allocator());
    REQUIRE(view.is_ok());
    CHECK(view.header->guid == MATERIAL_GUID);
    CHECK(view.header->type == ASSET_TYPE::MATERIAL);
    ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());

    u8* bytes = nullptr;
    usz size = 0;
    REQUIRE(TEXT_ASSET::read_file(path.c_str(), MEMORY::heap_allocator(), &bytes, &size));
    CHECK(std::string(reinterpret_cast<const char*>(bytes), size) == written(material));
    MEMORY::heap_allocator()->free(bytes);
    std::filesystem::remove(path);

    MaterialAsset no_guid;
    std::strcpy(no_guid.shader, "unlit");
    CHECK_FALSE(MATERIAL_ASSET::write_file(no_guid, path.c_str()));
    CHECK_FALSE(std::filesystem::exists(path));
}
