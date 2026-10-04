#include "engine/asset/asset_file.hpp"

#include "engine/memory/heap_allocator.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

// --- Errors -----------------------------------------------------------------------

const char* ASSET_FILE::parse_error_name(const AssetParseError error) {
    switch (error) {
    case ASSET_PARSE_OK: return "ok";
    case ASSET_PARSE_TOO_SMALL: return "buffer too small for the prelude";
    case ASSET_PARSE_BAD_MAGIC: return "not a .lunaasset file";
    case ASSET_PARSE_UNSUPPORTED_VERSION: return "unsupported format version";
    case ASSET_PARSE_BAD_HEADER: return "invalid header";
    case ASSET_PARSE_BAD_CHUNK: return "invalid chunk table";
    }
    return "unknown error";
}

// --- AssetView ----------------------------------------------------------------------

void AssetView::reset() {
    this->data = nullptr;
    this->size = 0;
    this->header = nullptr;
    this->dependencies = nullptr;
    this->chunks = nullptr;
}

AssetParseError AssetView::parse(const void* data, const usz size) {
    this->reset();

    if (data == nullptr || size < sizeof(AssetHeader)) {
        return ASSET_PARSE_TOO_SMALL;
    }
    ENGINE_ASSERT(reinterpret_cast<uintptr_t>(data) % alignof(AssetHeader) == 0,
                  "AssetView::parse: the buffer must be %zu-byte aligned (any allocator gives this)", alignof(AssetHeader));

    // Copy the header out rather than aliasing it: the buffer may not be
    // aligned for AssetHeader and the fields are checked before being trusted.
    AssetHeader header;
    std::memcpy(&header, data, sizeof(AssetHeader));

    if (header.magic != ASSET_FILE::MAGIC) {
        return ASSET_PARSE_BAD_MAGIC;
    }
    if (header.format_version != ASSET_FILE::FORMAT_VERSION) {
        return ASSET_PARSE_UNSUPPORTED_VERSION;
    }

    const usz prelude = ASSET_FILE::prelude_size(header.dependency_count, header.chunk_count);
    const usz payload_start = ASSET_FILE::payload_start(header.dependency_count, header.chunk_count);
    if (header.guid.is_null() || header.file_size < prelude || size > header.file_size) {
        return ASSET_PARSE_BAD_HEADER;
    }
    // A file with no payload bytes may end right after the tables, before the
    // alignment padding; otherwise the payload start must fit.
    if (header.chunk_count > 0 && header.file_size < payload_start) {
        return ASSET_PARSE_BAD_HEADER;
    }
    if (size < prelude) {
        return ASSET_PARSE_TOO_SMALL;
    }

    const u8* bytes = static_cast<const u8*>(data);
    const ChunkEntry* chunks = reinterpret_cast<const ChunkEntry*>(bytes + sizeof(AssetHeader) + sizeof(AssetGuid) * header.dependency_count);

    u64 previous_end = payload_start;
    for (u32 i = 0; i < header.chunk_count; ++i) {
        ChunkEntry chunk;
        std::memcpy(&chunk, chunks + i, sizeof(ChunkEntry));
        if (chunk.offset % ASSET_FILE::PAYLOAD_ALIGNMENT != 0) {
            return ASSET_PARSE_BAD_CHUNK;
        }
        if (chunk.offset < previous_end || chunk.offset > header.file_size || chunk.size > header.file_size - chunk.offset) {
            return ASSET_PARSE_BAD_CHUNK;
        }
        previous_end = chunk.offset + chunk.size;
    }

    this->data = bytes;
    this->size = size;
    this->header = reinterpret_cast<const AssetHeader*>(bytes);
    this->dependencies = reinterpret_cast<const AssetGuid*>(bytes + sizeof(AssetHeader));
    this->chunks = chunks;
    return ASSET_PARSE_OK;
}

const ChunkEntry* AssetView::find_chunk(const u32 tag) const {
    return this->find_chunk(tag, nullptr);
}

const ChunkEntry* AssetView::find_chunk(const u32 tag, const ChunkEntry* after) const {
    if (this->header == nullptr) {
        return nullptr;
    }
    const ChunkEntry* end = this->chunks + this->header->chunk_count;
    const ChunkEntry* begin = after != nullptr ? after + 1 : this->chunks;
    for (const ChunkEntry* chunk = begin; chunk < end; ++chunk) {
        if (chunk->tag == tag) {
            return chunk;
        }
    }
    return nullptr;
}

usz AssetView::find_dependency(const AssetGuid& guid) const {
    const usz count = this->dependency_count();
    for (usz i = 0; i < count; ++i) {
        if (this->dependencies[i] == guid) {
            return i;
        }
    }
    return count;
}

bool AssetView::has_editor_chunks() const {
    const usz count = this->chunk_count();
    for (usz i = 0; i < count; ++i) {
        if (this->chunks[i].is_editor_only()) {
            return true;
        }
    }
    return false;
}

// --- AssetWriter ----------------------------------------------------------------------

AssetWriter::AssetWriter() : AssetWriter(MEMORY::heap_allocator()) {}

AssetWriter::AssetWriter(BaseAllocator* allocator)
    : dependencies(allocator), chunks(allocator), body(allocator) {}

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

// read_prelude lives in asset_reader.cpp: it is an AssetReader open + read.

bool ASSET_FILE::write_file(const char* path, const void* data, const usz size) {
    FILE* file = fopen(path, "wb");
    if (file == nullptr) {
        fprintf(stderr, "[asset] error: cannot create %s\n", path);
        return false;
    }
    const bool ok = size == 0 || fwrite(data, 1, size, file) == size;
    if (!ok) {
        fprintf(stderr, "[asset] error: short write on %s\n", path);
    }
    return fclose(file) == 0 && ok;
}
