#include "import/importer.hpp"

#include "engine/asset/asset_reader.hpp"
#include "engine/asset/source_chunk.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/platform/file.hpp"
#include "engine/utils/hash.hpp"

#include <random>

ImportSource ImportSource::file(const std::filesystem::path& file) {
    ImportSource source;
    source.path = file;
    source.name = file.filename().string();
    return source;
}

std::string ImportSource::describe() const {
    if (this->is_embedded()) {
        return this->path.filename().string() + " (" + this->name + ")";
    }
    return this->name;
}

// Reads `size` bytes at `offset` of `path` (`size` 0: to the end) into a
// buffer from `allocator`. The shared body of read_file and read_source.
static u8* read_range(const char* path, const u64 offset, u64 size, BaseAllocator* allocator, usz* out_size, std::string* error) {
    File file;
    if (!PLATFORM::file_open(&file, path, FILE_ACCESS_READ)) {
        *error = std::string("cannot open ") + path;
        return nullptr;
    }
    u64 file_size = 0;
    if (!PLATFORM::file_size(file, &file_size) || offset > file_size) {
        *error = std::string("cannot read ") + path;
        PLATFORM::file_close(&file);
        return nullptr;
    }
    if (size == 0) {
        size = file_size - offset;
    } else if (size > file_size - offset) {
        *error = std::string("cannot read ") + path + ": the range lies past the end of the file";
        PLATFORM::file_close(&file);
        return nullptr;
    }
    if (size == 0) {
        *error = std::string("cannot read ") + path + ": the file is empty";
        PLATFORM::file_close(&file);
        return nullptr;
    }
    u8* bytes = static_cast<u8*>(allocator->allocate(size, 16));
    if (bytes == nullptr) {
        *error = "out of memory reading " + std::string(path);
        PLATFORM::file_close(&file);
        return nullptr;
    }
    const bool ok = PLATFORM::file_read(file, offset, bytes, size);
    PLATFORM::file_close(&file);
    if (!ok) {
        *error = std::string("short read on ") + path;
        allocator->free(bytes);
        return nullptr;
    }
    *out_size = size;
    return bytes;
}

u8* IMPORT::read_file(const char* path, BaseAllocator* allocator, usz* out_size, std::string* error) {
    return read_range(path, 0, 0, allocator, out_size, error);
}

u8* IMPORT::read_source(const ImportSource& source, BaseAllocator* allocator, usz* out_size, std::string* error) {
    const std::string path = source.path.string();
    if (source.is_embedded() && source.size == 0) {
        *error = source.describe() + ": the kept original is empty";
        return nullptr;
    }
    return read_range(path.c_str(), source.offset, source.size, allocator, out_size, error);
}

bool IMPORT::find_source(const std::filesystem::path& asset, ImportSource* out, std::string* error) {
    const std::string path = asset.string();
    AssetReader reader;
    if (!reader.open(path.c_str())) {
        *error = path + ": not a readable asset file";
        return false;
    }
    AssetView view = reader.read_prelude(MEMORY::heap_allocator());
    if (!view.is_ok()) {
        *error = path + ": " + ASSET_FILE::parse_error_name(view.parse_error);
        reader.close();
        return false;
    }

    bool found = false;
    const ChunkEntry* chunk = view.find_chunk(CHUNK_TYPE::SOURCE);
    if (chunk == nullptr) {
        *error = path + ": the asset has no source chunk";
    } else if (!SOURCE_CHUNK::has_source(chunk)) {
        *error = path + ": no original was kept";
    } else {
        u8 prefix[SOURCE_CHUNK::PREFIX_SIZE];
        const usz prefix_size = chunk->size < sizeof(prefix) ? static_cast<usz>(chunk->size) : sizeof(prefix);
        if (!reader.read_bytes(*chunk, 0, prefix, prefix_size)) {
            *error = path + ": cannot read the source chunk";
        } else {
            const SourceChunkView source = SourceChunkView::parse(*chunk, prefix, prefix_size);
            if (!source.is_ok()) {
                *error = path + ": " + SOURCE_CHUNK::parse_error_name(source.parse_error);
            } else {
                out->path = asset;
                out->offset = chunk->offset + source.data_offset;
                out->size = source.data_size;
                out->name.assign(source.name, source.name_size);
                out->guid = view.header->guid;
                found = true;
            }
        }
    }
    ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());
    reader.close();
    return found;
}

bool IMPORT::save_source(const ImportSource& source, const std::filesystem::path& destination, std::string* error) {
    usz size = 0;
    u8* bytes = IMPORT::read_source(source, MEMORY::heap_allocator(), &size, error);
    if (bytes == nullptr) {
        return false;
    }
    const std::string path = destination.string();
    const bool ok = ASSET_FILE::write_file(path.c_str(), bytes, size);
    MEMORY::heap_allocator()->free(bytes);
    if (!ok) {
        *error = "cannot write " + path;
    }
    return ok;
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

bool IMPORT::add_editor_chunks(AssetWriter& writer, const ImportSource& source, const void* bytes, const usz size, const bool keep_source,
                               std::string* error) {
    writer.add_chunk(CHUNK_TYPE::NAME, 0, CHUNK_FLAG::EDITOR_ONLY, nullptr, 0);
    writer.add_chunk(CHUNK_TYPE::IMPORT_SETTINGS, 0, CHUNK_FLAG::EDITOR_ONLY, nullptr, 0);
    if (!keep_source) {
        SOURCE_CHUNK::add_placeholder(writer);
        return true;
    }
    if (SOURCE_CHUNK::add_chunk(writer, source.name.c_str(), source.name.size(), bytes, size) == ~0u) {
        *error = source.describe() + ": cannot keep the original (the file name is empty or longer than " +
                 std::to_string(SOURCE_CHUNK::MAX_NAME_SIZE) + " bytes)";
        return false;
    }
    return true;
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
