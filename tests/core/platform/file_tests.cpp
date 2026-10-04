#include "support/test_support.hpp"

#include "engine/platform/file.hpp"

#include <cstring>
#include <filesystem>
#include <string>

namespace {

std::string temp_path(const char* stem) {
    return (std::filesystem::temp_directory_path() / (std::string("luau_engine_platform_") + stem + ".bin")).string();
}

// Writes `size` bytes of a recognisable pattern to `path` and returns them.
void write_pattern(const std::string& path, u8* bytes, const usz size) {
    for (usz i = 0; i < size; ++i) {
        bytes[i] = static_cast<u8>(i * 7 + 3);
    }
    File file;
    REQUIRE(PLATFORM::file_open(&file, path.c_str(), FILE_ACCESS_WRITE));
    REQUIRE(PLATFORM::file_write(file, 0, bytes, size));
    PLATFORM::file_close(&file);
}

} // namespace

TEST_CASE("platform/file: a default File is closed and every operation on it fails") {
    File file;
    CHECK_FALSE(file.is_open());

    u64 size = 0;
    u8 byte = 0;
    CHECK_FALSE(PLATFORM::file_size(file, &size));
    CHECK_FALSE(PLATFORM::file_read(file, 0, &byte, 1));
    CHECK_FALSE(PLATFORM::file_write(file, 0, &byte, 1));
    CHECK_FALSE(PLATFORM::file_read(file, 0, nullptr, 0));
    CHECK_FALSE(PLATFORM::file_write(file, 0, nullptr, 0));

    PLATFORM::file_close(&file); // closing a closed file is fine
    CHECK_FALSE(file.is_open());
}

TEST_CASE("platform/file: opening for read requires the file to exist") {
    File file;
    CHECK_FALSE(PLATFORM::file_open(&file, temp_path("does_not_exist/nested/missing").c_str(), FILE_ACCESS_READ));
    CHECK_FALSE(file.is_open());
    CHECK_FALSE(PLATFORM::file_open(&file, temp_path("missing").c_str(), FILE_ACCESS_READ));
    CHECK_FALSE(file.is_open());
}

TEST_CASE("platform/file: write creates the file and read gets the bytes back") {
    const std::string path = temp_path("roundtrip");
    std::filesystem::remove(path);

    u8 bytes[1000];
    write_pattern(path, bytes, sizeof(bytes));
    REQUIRE(std::filesystem::exists(path));

    File file;
    REQUIRE(PLATFORM::file_open(&file, path.c_str(), FILE_ACCESS_READ));
    CHECK(file.is_open());

    SUBCASE("size is what was written") {
        u64 size = 0;
        REQUIRE(PLATFORM::file_size(file, &size));
        CHECK(size == sizeof(bytes));
    }

    SUBCASE("the whole file") {
        u8 back[sizeof(bytes)];
        REQUIRE(PLATFORM::file_read(file, 0, back, sizeof(back)));
        CHECK(std::memcmp(back, bytes, sizeof(bytes)) == 0);
    }

    SUBCASE("reads are positional and independent of each other") {
        u8 tail[100];
        REQUIRE(PLATFORM::file_read(file, 900, tail, sizeof(tail)));
        CHECK(std::memcmp(tail, bytes + 900, sizeof(tail)) == 0);

        u8 middle[10];
        REQUIRE(PLATFORM::file_read(file, 495, middle, sizeof(middle)));
        CHECK(std::memcmp(middle, bytes + 495, sizeof(middle)) == 0);

        u8 head[4];
        REQUIRE(PLATFORM::file_read(file, 0, head, sizeof(head)));
        CHECK(std::memcmp(head, bytes, sizeof(head)) == 0);
    }

    SUBCASE("a zero-sized read succeeds anywhere, even past the end") {
        CHECK(PLATFORM::file_read(file, 0, nullptr, 0));
        CHECK(PLATFORM::file_read(file, sizeof(bytes), nullptr, 0));
        CHECK(PLATFORM::file_read(file, 1u << 20, nullptr, 0));
    }

    SUBCASE("a read that runs off the end fails") {
        u8 back[sizeof(bytes)];
        CHECK_FALSE(PLATFORM::file_read(file, 1, back, sizeof(back)));
        CHECK_FALSE(PLATFORM::file_read(file, sizeof(bytes), back, 1));
        CHECK_FALSE(PLATFORM::file_read(file, 1u << 20, back, 1));
    }

    SUBCASE("writing to a read-only file fails") {
        u8 byte = 1;
        CHECK_FALSE(PLATFORM::file_write(file, 0, &byte, 1));
    }

    PLATFORM::file_close(&file);
    CHECK_FALSE(file.is_open());
    std::filesystem::remove(path);
}

TEST_CASE("platform/file: opening for write truncates an existing file") {
    const std::string path = temp_path("truncate");
    u8 bytes[64];
    write_pattern(path, bytes, sizeof(bytes));

    File file;
    REQUIRE(PLATFORM::file_open(&file, path.c_str(), FILE_ACCESS_WRITE));

    SUBCASE("to zero bytes when nothing is written") {
        u64 size = ~0ull;
        REQUIRE(PLATFORM::file_size(file, &size));
        CHECK(size == 0);
        CHECK(PLATFORM::file_write(file, 0, nullptr, 0));
        REQUIRE(PLATFORM::file_size(file, &size));
        CHECK(size == 0);
    }

    SUBCASE("writes are positional and grow the file as needed") {
        const char first[] = "hello";
        const char second[] = "world";
        REQUIRE(PLATFORM::file_write(file, 0, first, 5));
        REQUIRE(PLATFORM::file_write(file, 8, second, 5)); // leaves a 3-byte hole
        u64 size = 0;
        REQUIRE(PLATFORM::file_size(file, &size));
        CHECK(size == 13);
        PLATFORM::file_close(&file);

        REQUIRE(PLATFORM::file_open(&file, path.c_str(), FILE_ACCESS_READ));
        char back[13];
        REQUIRE(PLATFORM::file_read(file, 0, back, sizeof(back)));
        CHECK(std::memcmp(back, "hello\0\0\0world", 13) == 0);
    }

    SUBCASE("reading from a write-only file fails") {
        u8 byte = 0;
        CHECK_FALSE(PLATFORM::file_read(file, 0, &byte, 1));
    }

    PLATFORM::file_close(&file);
    std::filesystem::remove(path);
}

TEST_CASE("platform/file: open on an open File closes the previous one first") {
    const std::string a = temp_path("reopen_a");
    const std::string b = temp_path("reopen_b");
    u8 bytes_a[16];
    u8 bytes_b[32];
    write_pattern(a, bytes_a, sizeof(bytes_a));
    write_pattern(b, bytes_b, sizeof(bytes_b));

    File file;
    REQUIRE(PLATFORM::file_open(&file, a.c_str(), FILE_ACCESS_READ));
    REQUIRE(PLATFORM::file_open(&file, b.c_str(), FILE_ACCESS_READ));
    u64 size = 0;
    REQUIRE(PLATFORM::file_size(file, &size));
    CHECK(size == sizeof(bytes_b));

    SUBCASE("a failed open leaves the File closed rather than on the old file") {
        CHECK_FALSE(PLATFORM::file_open(&file, temp_path("missing").c_str(), FILE_ACCESS_READ));
        CHECK_FALSE(file.is_open());
    }

    PLATFORM::file_close(&file);
    std::filesystem::remove(a);
    std::filesystem::remove(b);
}
