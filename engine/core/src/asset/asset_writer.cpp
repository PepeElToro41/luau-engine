#include "engine/asset/asset_writer.hpp"

#include "engine/memory/heap_allocator.hpp"
#include "engine/platform/file.hpp"

AssetWriter::AssetWriter() : AssetWriter(MEMORY::heap_allocator()) {}

AssetWriter::AssetWriter(BaseAllocator* allocator) : dependencies(allocator), chunks(allocator), body(allocator) {}

u32 AssetWriter::add_dependency(const AssetGuid& guid) {
    if (guid.is_null()) {
        fprintf(stderr, "[asset] error: a null guid cannot be a dependency\n");
        return ~0u;
    }
    for (usz i = 0; i < this->dependencies.count; ++i) {
        if (this->dependencies[i] == guid) {
            return static_cast<u32>(i);
        }
    }
    this->dependencies.push(guid);
    return static_cast<u32>(this->dependencies.count - 1);
}

u32 AssetWriter::add_chunk(const u32 tag, const u32 version, const u32 flags, const void* payload, const usz size) {
    // Each payload starts at an aligned offset relative to the body; the body
    // itself starts at an aligned file offset, so the file offsets are aligned too.
    const usz offset = ASSET_FILE::align_up(this->body.count, ASSET_FILE::PAYLOAD_ALIGNMENT);
    this->body.resize(offset + size);
    if (size > 0) {
        std::memcpy(this->body.data + offset, payload, size);
    }

    ChunkEntry chunk;
    chunk.tag = tag;
    chunk.version = version;
    chunk.flags = flags;
    chunk.offset = offset;
    chunk.size = size;
    this->chunks.push(chunk);
    return static_cast<u32>(this->chunks.count - 1);
}

usz AssetWriter::file_size() const {
    const u32 dependency_count = static_cast<u32>(this->dependencies.count);
    const u32 chunk_count = static_cast<u32>(this->chunks.count);
    if (chunk_count == 0) {
        return ASSET_FILE::prelude_size(dependency_count, 0);
    }
    return ASSET_FILE::payload_start(dependency_count, chunk_count) + this->body.count;
}

bool AssetWriter::write(void* out) const {
    if (this->guid.is_null()) {
        fprintf(stderr, "[asset] error: cannot write an asset with a null guid\n");
        return false;
    }

    const u32 dependency_count = static_cast<u32>(this->dependencies.count);
    const u32 chunk_count = static_cast<u32>(this->chunks.count);
    const usz prelude = ASSET_FILE::prelude_size(dependency_count, chunk_count);
    const usz payload_start = ASSET_FILE::payload_start(dependency_count, chunk_count);

    AssetHeader header;
    header.type = this->type;
    header.flags = this->flags;
    header.guid = this->guid;
    header.content_hash = this->content_hash;
    header.dependency_count = dependency_count;
    header.chunk_count = chunk_count;
    header.file_size = this->file_size();

    u8* cursor = static_cast<u8*>(out);
    std::memcpy(cursor, &header, sizeof(AssetHeader));
    cursor += sizeof(AssetHeader);

    if (dependency_count > 0) {
        std::memcpy(cursor, this->dependencies.data, sizeof(AssetGuid) * dependency_count);
        cursor += sizeof(AssetGuid) * dependency_count;
    }

    for (u32 i = 0; i < chunk_count; ++i) {
        ChunkEntry chunk = this->chunks[i];
        chunk.offset += payload_start;
        std::memcpy(cursor, &chunk, sizeof(ChunkEntry));
        cursor += sizeof(ChunkEntry);
    }

    if (chunk_count > 0) {
        std::memset(cursor, 0, payload_start - prelude);
        cursor = static_cast<u8*>(out) + payload_start;
        if (this->body.count > 0) {
            std::memcpy(cursor, this->body.data, this->body.count);
        }
    }
    return true;
}

u8* AssetWriter::write(BaseAllocator* allocator, usz* out_size) const {
    const usz size = this->file_size();
    u8* buffer = static_cast<u8*>(allocator->allocate(size, ASSET_FILE::PAYLOAD_ALIGNMENT));
    if (buffer == nullptr) {
        return nullptr;
    }
    if (!this->write(buffer)) {
        allocator->free(buffer);
        return nullptr;
    }
    if (out_size != nullptr) {
        *out_size = size;
    }
    return buffer;
}

void AssetWriter::clear() {
    this->dependencies.clear();
    this->chunks.clear();
    this->body.clear();
}

void AssetWriter::free() {
    this->dependencies.free();
    this->chunks.free();
    this->body.free();
}

// --- Files ----------------------------------------------------------------------------

bool ASSET_FILE::write_file(const char* path, const void* data, const usz size) {
    File file;
    if (!PLATFORM::file_open(&file, path, FILE_ACCESS_WRITE)) {
        fprintf(stderr, "[asset] error: cannot create %s\n", path);
        return false;
    }
    const bool ok = PLATFORM::file_write(file, 0, data, size);
    if (!ok) {
        fprintf(stderr, "[asset] error: short write on %s\n", path);
    }
    PLATFORM::file_close(&file);
    return ok;
}
