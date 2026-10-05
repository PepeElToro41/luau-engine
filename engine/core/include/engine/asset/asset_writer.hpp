#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

// Collects the parts of a .lunaasset file and emits them in the on-disk layout
// AssetView reads. Payload bytes are copied when added, so callers need not
// keep them alive. Chunks are written in the order they were added. Call
// free() when done.
//
//     AssetWriter writer;
//     writer.type = ASSET_TYPE::TEXTURE;
//     writer.guid = guid;
//     texture.add_chunks(writer); // a TextureAssetWriter, see asset_types/
//     usz size = 0;
//     u8* bytes = writer.write(allocator, &size);
//     ASSET_FILE::write_file(path, bytes, size);
struct AssetWriter {
    DynamicArray<AssetGuid> dependencies;
    DynamicArray<ChunkEntry> chunks; // `offset` is relative to the body start
    DynamicArray<u8> body;           // payloads, each at a PAYLOAD_ALIGNMENT-relative offset

    u32 type = 0;
    u32 flags = 0;
    AssetGuid guid;
    u64 content_hash = 0;

    AssetWriter();
    explicit AssetWriter(BaseAllocator* allocator);

    // Adds `guid` to the dependency table and returns its index; a GUID already
    // present returns its existing index. Null GUIDs are rejected (returns
    // ~0u) since a valid file never holds one.
    u32 add_dependency(const AssetGuid& guid);

    // Appends a chunk with a copy of `size` bytes at `payload`. `payload` may
    // be nullptr when `size` is 0. Returns the chunk's index.
    u32 add_chunk(u32 tag, u32 version, u32 flags, const void* payload, usz size);

    // Size in bytes of the file write() produces.
    usz file_size() const;

    // Allocates file_size() bytes on `allocator`, writes the file into them
    // and returns the buffer (caller frees it). nullptr on failure.
    u8* write(BaseAllocator* allocator, usz* out_size) const;

    // Writes the complete file into `out`, which must hold file_size() bytes.
    // Returns false and writes nothing if `guid` is null.
    bool write(void* out) const;

    // Forgets every dependency and chunk, keeping the header fields.
    void clear();
    // Releases the storage.
    void free();

    usz dependency_count() const { return this->dependencies.count; }
    usz chunk_count() const { return this->chunks.count; }
};

namespace ASSET_FILE {

// Writes `size` bytes to `path`, replacing any existing file. False (and a
// message on stderr) on failure.
bool write_file(const char* path, const void* data, usz size);

} // namespace ASSET_FILE
