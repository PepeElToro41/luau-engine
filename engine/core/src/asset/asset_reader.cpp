#include "engine/asset/asset_reader.hpp"


bool AssetReader::open(const char* path) {
    this->close();

    File file;
    if (!PLATFORM::file_open(&file, path, FILE_ACCESS_READ)) {
        fprintf(stderr, "[asset] error: cannot open %s\n", path);
        return false;
    }

    AssetHeader header;
    if (!PLATFORM::file_read(file, 0, &header, sizeof(AssetHeader)) || header.magic != ASSET_FILE::MAGIC ||
        header.format_version != ASSET_FILE::FORMAT_VERSION) {
        fprintf(stderr, "[asset] error: %s does not start with a valid asset header\n", path);
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
    if (!this->file.is_open() || !view.is_parsed()) {
        return false;
    }
    const AssetHeader& other = *view.header;
    return this->header.guid == other.guid && this->header.file_size == other.file_size && this->header.content_hash == other.content_hash;
}

u8* AssetReader::read_prelude(BaseAllocator* allocator, usz* out_size) {
    if (!this->file.is_open()) {
        fprintf(stderr, "[asset] error: read_prelude on a closed reader\n");
        return nullptr;
    }
    // Never read past the declared file size: a file with no payloads can
    // legitimately end before the alignment padding.
    usz size = ASSET_FILE::prelude_size(this->header.dependency_count, this->header.chunk_count);
    if (size > this->header.file_size) {
        size = this->header.file_size;
    }
    u8* buffer = static_cast<u8*>(allocator->allocate(size, ASSET_FILE::PAYLOAD_ALIGNMENT));
    if (buffer == nullptr) {
        fprintf(stderr, "[asset] error: out of memory reading a prelude (%llu bytes)\n", static_cast<unsigned long long>(size));
        return nullptr;
    }
    if (!PLATFORM::file_read(this->file, 0, buffer, size)) {
        fprintf(stderr, "[asset] error: short read on a prelude\n");
        allocator->free(buffer);
        return nullptr;
    }
    if (out_size != nullptr) {
        *out_size = size;
    }
    return buffer;
}

bool AssetReader::read_chunk(const ChunkEntry& chunk, void* out) {
    if (!this->file.is_open()) {
        fprintf(stderr, "[asset] error: read_chunk on a closed reader\n");
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

u8* AssetReader::read_chunk(const ChunkEntry& chunk, BaseAllocator* allocator) {
    if (chunk.size == 0) {
        // Still report a bad entry or a closed reader, as the other overload would.
        this->read_chunk(chunk, static_cast<void*>(nullptr));
        return nullptr;
    }
    u8* buffer = static_cast<u8*>(allocator->allocate(chunk.size, ASSET_FILE::PAYLOAD_ALIGNMENT));
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

u8* AssetReader::read_chunk(const AssetView& view, const u32 tag, BaseAllocator* allocator, const ChunkEntry** out_chunk) {
    if (out_chunk != nullptr) {
        *out_chunk = nullptr;
    }
    if (!this->matches(view)) {
        fprintf(stderr, "[asset] error: the view does not describe the open file; re-read its prelude\n");
        return nullptr;
    }
    const ChunkEntry* chunk = view.find_chunk(tag);
    if (chunk == nullptr) {
        return nullptr;
    }
    if (out_chunk != nullptr) {
        *out_chunk = chunk;
    }
    return this->read_chunk(*chunk, allocator);
}

u8* ASSET_FILE::read_prelude(const char* path, BaseAllocator* allocator, usz* out_size) {
    AssetReader reader;
    if (!reader.open(path)) {
        return nullptr;
    }
    u8* buffer = reader.read_prelude(allocator, out_size);
    reader.close();
    return buffer;
}
