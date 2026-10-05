#include "engine/asset/asset_view.hpp"

#include <cstdint>

// --- Errors -----------------------------------------------------------------------

const char* ASSET_FILE::parse_error_name(const AssetParseError error) {
    switch (error) {
    case ASSET_PARSE_OK: return "ok";
    case ASSET_FILE_ERROR: return "file could not be read";
    case ASSET_PARSE_TOO_SMALL: return "buffer too small for the prelude";
    case ASSET_PARSE_BAD_MAGIC: return "not a .lunaasset file";
    case ASSET_PARSE_UNSUPPORTED_VERSION: return "unsupported format version";
    case ASSET_PARSE_BAD_HEADER: return "invalid header";
    case ASSET_PARSE_BAD_CHUNK: return "invalid chunk table";
    }
    return "unknown error";
}

// --- AssetView ----------------------------------------------------------------------

AssetParseError AssetView::parse_header(const AssetHeader& header) {
    if (header.magic != ASSET_FILE::MAGIC) {
        return ASSET_PARSE_BAD_MAGIC;
    }
    if (header.format_version != ASSET_FILE::FORMAT_VERSION) {
        return ASSET_PARSE_UNSUPPORTED_VERSION;
    }
    if (header.guid.is_null() || header.file_size < ASSET_FILE::prelude_size(header)) {
        return ASSET_PARSE_BAD_HEADER;
    }
    // A file with no payload bytes may end right after the tables, before the
    // alignment padding; otherwise the payload start must fit.
    if (header.chunk_count > 0 && header.file_size < ASSET_FILE::payload_start(header)) {
        return ASSET_PARSE_BAD_HEADER;
    }
    return ASSET_PARSE_OK;
}

AssetView AssetView::parse(const void* data, const usz size) {
    if (data == nullptr || size < sizeof(AssetHeader)) {
        return AssetView{ASSET_PARSE_TOO_SMALL};
    }
    ENGINE_ASSERT(reinterpret_cast<uintptr_t>(data) % alignof(AssetHeader) == 0,
                  "AssetView::parse: the buffer must be %zu-byte aligned (any allocator gives this)", alignof(AssetHeader));

    // Copy the header out rather than aliasing it: the fields are checked
    // before being trusted.
    AssetHeader header;
    std::memcpy(&header, data, sizeof(AssetHeader));
    if (const AssetParseError error = AssetView::parse_header(header); error != ASSET_PARSE_OK) {
        return AssetView{error};
    }

    const usz prelude = ASSET_FILE::prelude_size(header);
    const usz payload_start = ASSET_FILE::payload_start(header);
    // A buffer longer than the declared file is not this file.
    if (size > header.file_size) {
        return AssetView{ASSET_PARSE_BAD_HEADER};
    }
    if (size < prelude) {
        return AssetView{ASSET_PARSE_TOO_SMALL};
    }

    const u8* bytes = static_cast<const u8*>(data);
    const ChunkEntry* chunks = reinterpret_cast<const ChunkEntry*>(bytes + sizeof(AssetHeader) + sizeof(AssetGuid) * header.dependency_count);

    u64 previous_end = payload_start;
    for (u32 i = 0; i < header.chunk_count; ++i) {
        ChunkEntry chunk;
        std::memcpy(&chunk, chunks + i, sizeof(ChunkEntry));
        if (chunk.offset % ASSET_FILE::PAYLOAD_ALIGNMENT != 0) {
            return AssetView{ASSET_PARSE_BAD_CHUNK};
        }
        if (chunk.offset < previous_end || chunk.offset > header.file_size || chunk.size > header.file_size - chunk.offset) {
            return AssetView{ASSET_PARSE_BAD_CHUNK};
        }
        previous_end = chunk.offset + chunk.size;
    }

    AssetView view;
    view.data = bytes;
    view.size = size;
    view.header = reinterpret_cast<const AssetHeader*>(bytes);
    view.dependencies = reinterpret_cast<const AssetGuid*>(bytes + sizeof(AssetHeader));
    view.chunks = chunks;
    return view;
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
        if (this->chunks[i].editor_only()) {
            return true;
        }
    }
    return false;
}
