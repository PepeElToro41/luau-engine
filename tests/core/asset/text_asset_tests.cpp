#include "support/test_support.hpp"

#include "engine/asset/asset_reader.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/utils/hash.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

const AssetGuid SAMPLE_GUID = {0x179a52dd06b33f96ull, 0xe26f6a98c55cbe22ull};
constexpr const char* SAMPLE_GUID_TEXT = "e26f6a98c55cbe22179a52dd06b33f96";

std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / (std::string("luau_engine_text_") + name)).string();
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

struct Entry {
    std::string section;
    std::string key;
    std::string value;
    u32 line = 0;
};

// Every entry of `text`, in order; `error` receives the cursor's error.
DynamicArray<Entry> walk(const std::string& text, const char** error, u32* error_line) {
    DynamicArray<Entry> entries;
    TextAssetCursor cursor(text.data(), text.size());
    TextAssetEntry entry;
    while (cursor.next(&entry)) {
        entries.push(Entry{std::string(entry.section, entry.section_length), std::string(entry.key, entry.key_length),
            std::string(entry.value, entry.value_length), entry.line});
    }
    *error = cursor.error;
    *error_line = cursor.line;
    return entries;
}

} // namespace

TEST_CASE("asset/text_asset: type_of_path names the type by extension") {
    CHECK(TEXT_ASSET::type_of_path("materials/rock.material") == ASSET_TYPE::MATERIAL);
    CHECK(TEXT_ASSET::type_of_path("ROCK.MATERIAL") == ASSET_TYPE::MATERIAL);
    CHECK(TEXT_ASSET::type_of_path("rock.lunaasset") == 0);
    CHECK(TEXT_ASSET::type_of_path("material") == 0);
    CHECK(TEXT_ASSET::type_of_path("") == 0);
    CHECK(TEXT_ASSET::type_of_path(nullptr) == 0);
    CHECK(std::strcmp(TEXT_ASSET::extension_of_type(ASSET_TYPE::MATERIAL), ".material") == 0);
    CHECK(TEXT_ASSET::extension_of_type(ASSET_TYPE::MESH) == nullptr);
}

TEST_CASE("asset/text_asset: guids round-trip through text as hi then lo") {
    char text[TEXT_ASSET::GUID_TEXT_CAPACITY];
    TEXT_ASSET::format_guid(SAMPLE_GUID, text);
    CHECK(std::string(text) == SAMPLE_GUID_TEXT);

    AssetGuid parsed;
    REQUIRE(TEXT_ASSET::parse_guid(text, std::strlen(text), &parsed));
    CHECK(parsed == SAMPLE_GUID);

    SUBCASE("either case and dashes are accepted") {
        const char dashed[] = "E26F6A98-C55CBE22-179A52DD-06B33F96";
        REQUIRE(TEXT_ASSET::parse_guid(dashed, std::strlen(dashed), &parsed));
        CHECK(parsed == SAMPLE_GUID);
    }
    SUBCASE("anything else is refused and leaves the output alone") {
        parsed = SAMPLE_GUID;
        const char* bad[] = {"e26f6a98c55cbe22179a52dd06b33f9", "e26f6a98c55cbe22179a52dd06b33f960", "g26f6a98c55cbe22179a52dd06b33f96",
            "00000000000000000000000000000000", ""};
        for (const char* input : bad) {
            CHECK_FALSE(TEXT_ASSET::parse_guid(input, std::strlen(input), &parsed));
            CHECK(parsed == SAMPLE_GUID);
        }
    }
}

TEST_CASE("asset/text_asset: the cursor walks key = value lines and sections") {
    const std::string text = "# comment\r\n"
                             "guid = abc   # trailing comment\n"
                             "  shader=unlit\n"
                             "\n"
                             "[params]\n"
                             "color = 1 0.5 0.15 1\n"
                             "label = \"has # inside\" # after\n"
                             "empty =\n"
                             "[ textures ] # section comment\n"
                             "albedo = none\n";
    const char* error = nullptr;
    u32 error_line = 0;
    DynamicArray<Entry> entries = walk(text, &error, &error_line);
    CHECK(error == nullptr);
    REQUIRE(entries.count == 6);
    CHECK(entries[0].section == "");
    CHECK(entries[0].key == "guid");
    CHECK(entries[0].value == "abc");
    CHECK(entries[0].line == 2);
    CHECK(entries[1].key == "shader");
    CHECK(entries[1].value == "unlit");
    CHECK(entries[2].section == "params");
    CHECK(entries[2].key == "color");
    CHECK(entries[2].value == "1 0.5 0.15 1");
    CHECK(entries[2].line == 6);
    CHECK(entries[3].value == "has # inside");
    CHECK(entries[4].key == "empty");
    CHECK(entries[4].value == "");
    CHECK(entries[5].section == "textures");
    CHECK(entries[5].value == "none");
    entries.free();

    SUBCASE("entry helpers compare by span") {
        TextAssetCursor cursor(text.data(), text.size());
        TextAssetEntry entry;
        REQUIRE(cursor.next(&entry));
        CHECK(entry.in_top_level());
        CHECK(entry.key_is("guid"));
        CHECK_FALSE(entry.key_is("gui"));
        CHECK_FALSE(entry.key_is("guids"));
        CHECK(entry.value_is("abc"));
    }
}

TEST_CASE("asset/text_asset: the cursor stops on malformed lines with the line number") {
    const char* error = nullptr;
    u32 error_line = 0;
    SUBCASE("a line without =") {
        DynamicArray<Entry> entries = walk("guid = a\nnot a pair\nshader = b\n", &error, &error_line);
        CHECK(entries.count == 1);
        CHECK(error != nullptr);
        CHECK(error_line == 2);
        entries.free();
    }
    SUBCASE("an unterminated section") {
        DynamicArray<Entry> entries = walk("[params\n", &error, &error_line);
        CHECK(entries.count == 0);
        CHECK(error != nullptr);
        CHECK(error_line == 1);
        entries.free();
    }
    SUBCASE("an empty key") {
        DynamicArray<Entry> entries = walk("= value\n", &error, &error_line);
        CHECK(error != nullptr);
        entries.free();
    }
    SUBCASE("an unterminated quote") {
        DynamicArray<Entry> entries = walk("name = \"open\n", &error, &error_line);
        CHECK(error != nullptr);
        entries.free();
    }
    SUBCASE("a clean end has no error") {
        DynamicArray<Entry> entries = walk("a = 1", &error, &error_line);
        CHECK(entries.count == 1);
        CHECK(error == nullptr);
        entries.free();
    }
}

TEST_CASE("asset/text_asset: parse_numbers splits on spaces and commas") {
    f64 numbers[4];
    usz count = 0;
    const char a[] = "1 0.5, -2e3,4";
    REQUIRE(TEXT_ASSET::parse_numbers(a, std::strlen(a), numbers, 4, &count));
    REQUIRE(count == 4);
    CHECK(numbers[0] == 1.0);
    CHECK(numbers[1] == 0.5);
    CHECK(numbers[2] == -2000.0);
    CHECK(numbers[3] == 4.0);

    REQUIRE(TEXT_ASSET::parse_numbers("", 0, numbers, 4, &count));
    CHECK(count == 0);

    const char word[] = "1 two";
    CHECK_FALSE(TEXT_ASSET::parse_numbers(word, std::strlen(word), numbers, 4, &count));
    const char many[] = "1 2 3 4 5";
    CHECK_FALSE(TEXT_ASSET::parse_numbers(many, std::strlen(many), numbers, 4, &count));
}

TEST_CASE("asset/text_asset: build_prelude describes the text as one cooked TEXT chunk") {
    const std::string text = "guid = " + std::string(SAMPLE_GUID_TEXT) + "\nshader = unlit\n";
    AssetView view = TEXT_ASSET::build_prelude(ASSET_TYPE::MATERIAL, SAMPLE_GUID, text.data(), text.size(), MEMORY::heap_allocator());
    REQUIRE(view.is_ok());
    CHECK(view.is_text());
    CHECK(view.is_cooked());
    CHECK(view.header->type == ASSET_TYPE::MATERIAL);
    CHECK(view.header->guid == SAMPLE_GUID);
    CHECK(view.header->content_hash == HASH::fnv1a(text.data(), text.size()));
    CHECK(view.dependency_count() == 0);
    REQUIRE(view.chunk_count() == 1);
    const ChunkEntry* chunk = view.find_chunk(CHUNK_TYPE::TEXT);
    REQUIRE(chunk != nullptr);
    CHECK(chunk->version == TEXT_ASSET::VERSION);
    CHECK(chunk->size == text.size());
    CHECK(chunk->offset == ASSET_FILE::payload_start(0, 1));
    CHECK(view.header->file_size == chunk->offset + text.size());
    CHECK_FALSE(chunk->editor_only());
    CHECK(TEXT_ASSET::matches(view, text.data(), text.size()));
    CHECK_FALSE(TEXT_ASSET::matches(view, text.data(), text.size() - 1));
    ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());

    SUBCASE("a null guid or a binary type is refused") {
        AssetView bad = TEXT_ASSET::build_prelude(ASSET_TYPE::MATERIAL, AssetGuid{}, text.data(), text.size(), MEMORY::heap_allocator());
        CHECK_FALSE(bad.is_ok());
        CHECK(bad.parse_error == ASSET_PARSE_BAD_HEADER);
        bad = TEXT_ASSET::build_prelude(ASSET_TYPE::MESH, SAMPLE_GUID, text.data(), text.size(), MEMORY::heap_allocator());
        CHECK_FALSE(bad.is_ok());
    }
    SUBCASE("from a path the guid comes from the text and the type from the extension") {
        AssetView from_path = TEXT_ASSET::build_prelude("rock.material", text.data(), text.size(), MEMORY::heap_allocator());
        REQUIRE(from_path.is_ok());
        CHECK(from_path.header->guid == SAMPLE_GUID);
        CHECK(from_path.header->type == ASSET_TYPE::MATERIAL);
        ASSET_FILE::free_prelude(&from_path, MEMORY::heap_allocator());

        const std::string no_guid = "shader = unlit\n";
        CHECK_FALSE(TEXT_ASSET::build_prelude("rock.material", no_guid.data(), no_guid.size(), MEMORY::heap_allocator()).is_ok());
        const std::string guid_after_section = "[params]\nguid = " + std::string(SAMPLE_GUID_TEXT) + "\n";
        CHECK_FALSE(TEXT_ASSET::build_prelude("rock.material", guid_after_section.data(), guid_after_section.size(), MEMORY::heap_allocator()).is_ok());
        CHECK_FALSE(TEXT_ASSET::build_prelude("rock.lunaasset", text.data(), text.size(), MEMORY::heap_allocator()).is_ok());
    }
}

TEST_CASE("asset/text_asset: find_guid reports the line of a malformed guid") {
    AssetGuid guid;
    u32 line = 0;
    const std::string good = "# header\nguid = " + std::string(SAMPLE_GUID_TEXT) + "\n";
    REQUIRE(TEXT_ASSET::find_guid(good.data(), good.size(), &guid, &line));
    CHECK(guid == SAMPLE_GUID);
    CHECK(line == 2);

    const std::string bad = "shader = x\nguid = nope\n";
    CHECK_FALSE(TEXT_ASSET::find_guid(bad.data(), bad.size(), &guid, &line));
    CHECK(line == 2);

    const std::string missing = "shader = x\n";
    CHECK_FALSE(TEXT_ASSET::find_guid(missing.data(), missing.size(), &guid, &line));
    CHECK(line == 0);
}

TEST_CASE("asset/text_asset: read_prelude scans a file and read_prelude_any dispatches") {
    const std::string path = temp_path("scan.material");
    const std::string text = "guid = " + std::string(SAMPLE_GUID_TEXT) + "\nshader = unlit\n";
    write_text(path, text);

    AssetView view = TEXT_ASSET::read_prelude(path.c_str(), MEMORY::heap_allocator());
    REQUIRE(view.is_ok());
    CHECK(view.is_text());
    CHECK(view.header->guid == SAMPLE_GUID);
    CHECK(view.find_chunk(CHUNK_TYPE::TEXT)->size == text.size());
    ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());

    AssetView any = ASSET_FILE::read_prelude_any(path.c_str(), MEMORY::heap_allocator());
    REQUIRE(any.is_ok());
    CHECK(any.is_text());
    ASSET_FILE::free_prelude(&any, MEMORY::heap_allocator());

    SUBCASE("read_file returns the bytes") {
        u8* bytes = nullptr;
        usz size = 0;
        REQUIRE(TEXT_ASSET::read_file(path.c_str(), MEMORY::heap_allocator(), &bytes, &size));
        REQUIRE(size == text.size());
        CHECK(std::memcmp(bytes, text.data(), size) == 0);
        MEMORY::heap_allocator()->free(bytes);
    }
    SUBCASE("a missing file is a file error") {
        const std::string missing = temp_path("missing.material");
        std::filesystem::remove(missing);
        AssetView none = TEXT_ASSET::read_prelude(missing.c_str(), MEMORY::heap_allocator());
        CHECK_FALSE(none.is_ok());
        CHECK(none.parse_error == ASSET_FILE_ERROR);
        u8* bytes = nullptr;
        usz size = 0;
        CHECK_FALSE(TEXT_ASSET::read_file(missing.c_str(), MEMORY::heap_allocator(), &bytes, &size));
        CHECK(bytes == nullptr);
    }
    std::filesystem::remove(path);
}
