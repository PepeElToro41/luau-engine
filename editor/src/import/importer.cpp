#include "import/importer.hpp"

#include "engine/utils/hash.hpp"

#include "engine/memory/heap_allocator.hpp"
#include "engine/platform/file.hpp"

#include <random>

u8* IMPORT::read_file(const char* path, BaseAllocator* allocator, usz* out_size, std::string* error) {
    File file;
    if (!PLATFORM::file_open(&file, path, FILE_ACCESS_READ)) {
        *error = std::string("cannot open ") + path;
        return nullptr;
    }
    u64 size = 0;
    if (!PLATFORM::file_size(file, &size) || size == 0) {
        *error = std::string("cannot read ") + path + (size == 0 ? ": the file is empty" : "");
        PLATFORM::file_close(&file);
        return nullptr;
    }
    u8* bytes = static_cast<u8*>(allocator->allocate(size, 16));
    if (bytes == nullptr) {
        *error = "out of memory reading " + std::string(path);
        PLATFORM::file_close(&file);
        return nullptr;
    }
    const bool ok = PLATFORM::file_read(file, 0, bytes, size);
    PLATFORM::file_close(&file);
    if (!ok) {
        *error = std::string("short read on ") + path;
        allocator->free(bytes);
        return nullptr;
    }
    *out_size = size;
    return bytes;
}

u64 IMPORT::fnv1a(const void* data, const usz size) {
    return HASH::fnv1a(data, size);
}

AssetGuid IMPORT::random_guid() {
    static std::mt19937_64 generator{std::random_device{}()};
    AssetGuid guid;
    do {
        guid.lo = generator();
        guid.hi = generator();
    } while (guid.is_null());
    return guid;
}

void IMPORT::add_editor_chunks(AssetWriter& writer) {
    writer.add_chunk(CHUNK_TYPE::NAME, 0, CHUNK_FLAG::EDITOR_ONLY, nullptr, 0);
    writer.add_chunk(CHUNK_TYPE::IMPORT_SETTINGS, 0, CHUNK_FLAG::EDITOR_ONLY, nullptr, 0);
    writer.add_chunk(CHUNK_TYPE::SOURCE, 0, CHUNK_FLAG::EDITOR_ONLY, nullptr, 0);
}

bool IMPORT::write_asset(const AssetWriter& writer, const char* path, std::string* error) {
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    if (bytes == nullptr) {
        *error = "the asset has no guid or could not be laid out";
        return false;
    }
    const bool ok = ASSET_FILE::write_file(path, bytes, size);
    MEMORY::heap_allocator()->free(bytes);
    if (!ok) {
        *error = std::string("cannot write ") + path;
    }
    return ok;
}
