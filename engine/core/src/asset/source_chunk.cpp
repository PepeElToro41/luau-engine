#include "engine/asset/source_chunk.hpp"

#include <cstring>

bool SOURCE_CHUNK::has_source(const ChunkEntry* entry) {
    return entry != nullptr && entry->tag == CHUNK_TYPE::SOURCE && entry->version == SOURCE_CHUNK::VERSION &&
           entry->size > sizeof(SourceChunkHeader);
}

bool SOURCE_CHUNK::has_source(const AssetView& view) {
    return SOURCE_CHUNK::has_source(view.find_chunk(CHUNK_TYPE::SOURCE));
}

u32 SOURCE_CHUNK::add_chunk(AssetWriter& writer, const char* name, const usz name_size, const void* data, const usz data_size) {
    if (name == nullptr || name_size == 0 || name_size > SOURCE_CHUNK::MAX_NAME_SIZE || (data == nullptr && data_size != 0)) {
        return ~0u;
    }
    const usz offset = SOURCE_CHUNK::data_offset(name_size);
    SourceChunkHeader header;
    header.name_size = static_cast<u32>(name_size);
    header.data_size = data_size;

    // Added as an empty chunk and grown in place: the writer appends
    // payloads to `body` in order, so the chunk just added ends the body
    // and its bytes can follow without staging the (possibly large) blob
    // in a second buffer.
    const u32 index = writer.add_chunk(CHUNK_TYPE::SOURCE, SOURCE_CHUNK::VERSION, CHUNK_FLAG::EDITOR_ONLY, nullptr, 0);
    ChunkEntry& entry = writer.chunks[index];
    const usz start = entry.offset;
    writer.body.resize(start + offset + data_size);
    u8* payload = writer.body.data + start;
    memset(payload, 0, offset);
    memcpy(payload, &header, sizeof(header));
    memcpy(payload + sizeof(header), name, name_size);
    if (data_size != 0) {
        memcpy(payload + offset, data, data_size);
    }
    entry.size = offset + data_size;
    return index;
}

u32 SOURCE_CHUNK::add_chunk(AssetWriter& writer, const char* name, const void* data, const usz data_size) {
    return SOURCE_CHUNK::add_chunk(writer, name, name != nullptr ? strlen(name) : 0, data, data_size);
}

u32 SOURCE_CHUNK::add_placeholder(AssetWriter& writer) {
    return writer.add_chunk(CHUNK_TYPE::SOURCE, 0, CHUNK_FLAG::EDITOR_ONLY, nullptr, 0);
}

const char* SOURCE_CHUNK::parse_error_name(const SourceParseError error) {
    switch (error) {
    case SOURCE_PARSE_OK: return "ok";
    case SOURCE_PARSE_NO_SOURCE: return "no source kept";
    case SOURCE_PARSE_UNSUPPORTED_VERSION: return "unsupported source chunk version";
    case SOURCE_PARSE_TOO_SMALL: return "buffer too small for the source chunk prefix";
    case SOURCE_PARSE_BAD_NAME: return "bad source name size";
    case SOURCE_PARSE_BAD_SIZE: return "source chunk size does not match its header";
    }
    return "unknown";
}

SourceChunkView SourceChunkView::parse(const ChunkEntry& entry, const void* payload, const usz size) {
    if (entry.size == 0) {
        return SourceChunkView{SOURCE_PARSE_NO_SOURCE};
    }
    if (entry.version != SOURCE_CHUNK::VERSION) {
        return SourceChunkView{SOURCE_PARSE_UNSUPPORTED_VERSION};
    }
    if (payload == nullptr || size < sizeof(SourceChunkHeader) || entry.size < sizeof(SourceChunkHeader)) {
        return SourceChunkView{SOURCE_PARSE_TOO_SMALL};
    }
    const u8* bytes = static_cast<const u8*>(payload);
    SourceChunkHeader header;
    memcpy(&header, bytes, sizeof(header)); // the buffer may be unaligned (a prefix read anywhere)
    if (header.name_size == 0 || header.name_size > SOURCE_CHUNK::MAX_NAME_SIZE) {
        return SourceChunkView{SOURCE_PARSE_BAD_NAME};
    }
    const usz offset = SOURCE_CHUNK::data_offset(header.name_size);
    if (entry.size != offset + header.data_size) {
        return SourceChunkView{SOURCE_PARSE_BAD_SIZE};
    }
    if (size < sizeof(SourceChunkHeader) + header.name_size) {
        return SourceChunkView{SOURCE_PARSE_TOO_SMALL};
    }

    SourceChunkView view;
    view.header = reinterpret_cast<const SourceChunkHeader*>(bytes);
    view.name = reinterpret_cast<const char*>(bytes + sizeof(SourceChunkHeader));
    view.name_size = header.name_size;
    view.data_offset = offset;
    view.data_size = header.data_size;
    if (size >= entry.size) {
        view.data = bytes + offset;
    }
    return view;
}
