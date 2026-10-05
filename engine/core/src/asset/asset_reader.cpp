#include "engine/asset/asset_reader.hpp"

namespace {

bool check_open(const AssetReader& reader, const char* what) {
    if (!reader.is_open()) {
        fprintf(stderr, "[asset] error: %s on a closed reader\n", what);
        return false;
    }
    return true;
}

} // namespace

bool AssetReader::open(const char* path) {
    this->close();

    File file;
    if (!PLATFORM::file_open(&file, path, FILE_ACCESS_READ)) {
        fprintf(stderr, "[asset] error: cannot open %s\n", path);
        return false;
    }

    AssetHeader header;
    if (!PLATFORM::file_read(file, 0, &header, sizeof(AssetHeader))) {
        fprintf(stderr, "[asset] error: %s is shorter than an asset header\n", path);
        PLATFORM::file_close(&file);
        return false;
    }
    if (const AssetParseError error = AssetView::parse_header(header); error != ASSET_PARSE_OK) {
        fprintf(stderr, "[asset] error: %s does not start with a valid asset header (%s)\n", path, ASSET_FILE::parse_error_name(error));
        PLATFORM::file_close(&file);
        return false;
    }
    u64 on_disk = 0;
    if (!PLATFORM::file_size(file, &on_disk)) {
        fprintf(stderr, "[asset] error: cannot read %s\n", path);
        PLATFORM::file_close(&file);
        return false;
    }
    if (on_disk != header.file_size) {
        fprintf(stderr, "[asset] error: %s is %llu bytes but its header says %llu\n", path, static_cast<unsigned long long>(on_disk),
                static_cast<unsigned long long>(header.file_size));
        PLATFORM::file_close(&file);
        return false;
    }

    this->file = file;
    this->header = header;
    return true;
}

void AssetReader::close() {
    PLATFORM::file_close(&this->file);
    this->header = AssetHeader{};
}

bool AssetReader::matches(const AssetView& view) const {
    if (!this->is_open() || !view.is_ok()) {
        return false;
    }
    const AssetHeader& other = *view.header;
    return this->header.guid == other.guid && this->header.file_size == other.file_size && this->header.content_hash == other.content_hash;
}

AssetView AssetReader::read_prelude(BaseAllocator* allocator) {
    if (!check_open(*this, "read_prelude")) {
        return AssetView{ASSET_FILE_ERROR};
    }
    // Never read past the declared file size: a file with no payloads can
    // legitimately end before the alignment padding.
    usz size = ASSET_FILE::prelude_size(this->header);
    if (size > this->header.file_size) {
        size = this->header.file_size;
    }
    u8* buffer = static_cast<u8*>(allocator->allocate(size, ASSET_FILE::PAYLOAD_ALIGNMENT));
    if (buffer == nullptr) {
        fprintf(stderr, "[asset] error: out of memory reading a prelude (%llu bytes)\n", static_cast<unsigned long long>(size));
        return AssetView{ASSET_FILE_ERROR};
    }
    if (!PLATFORM::file_read(this->file, 0, buffer, size)) {
        fprintf(stderr, "[asset] error: short read on a prelude\n");
        allocator->free(buffer);
        return AssetView{ASSET_FILE_ERROR};
    }
    AssetView view = AssetView::parse(buffer, size);
    if (!view.is_ok()) {
        allocator->free(buffer);
    }
    return view;
}

bool AssetReader::read_chunk(const ChunkEntry& chunk, void* out) {
    if (!check_open(*this, "read_chunk")) {
        return false;
    }
    if (chunk.offset % ASSET_FILE::PAYLOAD_ALIGNMENT != 0 || chunk.offset > this->header.file_size ||
        chunk.size > this->header.file_size - chunk.offset) {
        fprintf(stderr, "[asset] error: chunk at %llu (+%llu) lies outside the %llu-byte file\n", static_cast<unsigned long long>(chunk.offset),
                static_cast<unsigned long long>(chunk.size), static_cast<unsigned long long>(this->header.file_size));
        return false;
    }
    if (chunk.size == 0) {
        return true;
    }
    if (!PLATFORM::file_read(this->file, chunk.offset, out, chunk.size)) {
        fprintf(stderr, "[asset] error: short read on chunk at %llu (+%llu)\n", static_cast<unsigned long long>(chunk.offset),
                static_cast<unsigned long long>(chunk.size));
        return false;
    }
    return true;
}

void* AssetReader::read_chunk(const ChunkEntry& chunk, BaseAllocator* allocator) {
    if (chunk.size == 0) {
        // Still report a bad entry or a closed reader, as the other overload would.
        this->read_chunk(chunk, static_cast<void*>(nullptr));
        return nullptr;
    }
    void* buffer = allocator->allocate(chunk.size, ASSET_FILE::PAYLOAD_ALIGNMENT);
    if (buffer == nullptr) {
        fprintf(stderr, "[asset] error: out of memory reading a chunk (%llu bytes)\n", static_cast<unsigned long long>(chunk.size));
        return nullptr;
    }
    if (!this->read_chunk(chunk, buffer)) {
        allocator->free(buffer);
        return nullptr;
    }
    return buffer;
}

ReadChunk AssetReader::read_chunk(const AssetView& view, const u32 tag, BaseAllocator* allocator) {
    if (!check_open(*this, "read_chunk")) {
        return ReadChunk{ASSET_READ_INVALID};
    }
    if (!this->matches(view)) {
        fprintf(stderr, "[asset] error: the view does not describe the open file; re-read its prelude\n");
        return ReadChunk{ASSET_READ_STALE_VIEW};
    }
    const ChunkEntry* chunk = view.find_chunk(tag);
    if (chunk == nullptr) {
        return ReadChunk{ASSET_READ_NOT_FOUND};
    }
    if (chunk->size == 0) {
        return ReadChunk{nullptr, *chunk};
    }
    void* data = this->read_chunk(*chunk, allocator);
    if (data == nullptr) {
        return ReadChunk{ASSET_READ_FILE_ERROR};
    }
    return ReadChunk{data, *chunk};
}

// --- Files ----------------------------------------------------------------------------

AssetView ASSET_FILE::read_prelude(const char* path, BaseAllocator* allocator) {
    AssetReader reader;
    if (!reader.open(path)) {
        return AssetView{ASSET_FILE_ERROR};
    }
    AssetView view = reader.read_prelude(allocator);
    reader.close();
    return view;
}

void ASSET_FILE::free_prelude(AssetView* view, BaseAllocator* allocator) {
    // The view points into a buffer read_prelude allocated; it is the owner
    // in all but constness.
    allocator->free(const_cast<u8*>(view->data));
    *view = AssetView{};
}
