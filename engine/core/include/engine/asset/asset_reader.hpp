#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/platform/file.hpp"

// Reads chunk payloads out of a .lunaasset file on demand. An AssetView
// (from read_prelude, typically scanned long before anything is loaded) says
// where every chunk is; the reader opens the file and seeks straight to the
// chunks asked for, so loading a mesh's bounds costs one 48-byte read and
// nothing else in the file is touched.
//
//     AssetReader reader;
//     if (reader.open("rock.lunaasset")) {
//         ReadChunk mesh = reader.read_chunk(view, CHUNK_TYPE::MESH, allocator);
//         if (mesh.is_ok()) { ... mesh.chunk_data, mesh.entry.size ... }
//         allocator->free(mesh.chunk_data);
//     }
//     reader.close();
//
// open() reads the header back and checks it against the file on disk, and
// matches() checks it against the view, so a view from a stale scan (the
// file was re-imported since) is caught before any chunk is trusted; the
// caller then re-reads the prelude. The reader holds an open platform File
// and the header it read; close() when done.

enum AssetReadError {
    ASSET_READ_INVALID = 0,  // nothing was read: the reader is closed or the chunk was never requested
    ASSET_READ_FILE_ERROR,   // the entry lies outside the file, allocation failed or the read came up short
    ASSET_READ_NOT_FOUND,    // the view has no chunk with that tag
    ASSET_READ_STALE_VIEW,   // the view does not describe the open file; re-read its prelude
    ASSET_READ_OK,
};

// One chunk read by tag: the payload bytes and the table entry they belong
// to, so the caller knows the size and version without going back to the view.
struct ReadChunk {
    AssetReadError read_error = ASSET_READ_INVALID;

    // The payload, in a PAYLOAD_ALIGNMENT aligned buffer the caller frees.
    // nullptr for a size 0 chunk, which has nothing to read but still is_ok().
    void* chunk_data = nullptr;
    ChunkEntry entry{};

    ReadChunk() = default;
    explicit ReadChunk(const AssetReadError error) : read_error(error) {}
    ReadChunk(void* chunk_data, const ChunkEntry& entry) : read_error(ASSET_READ_OK), chunk_data(chunk_data), entry(entry) {}

    bool is_ok() const { return this->read_error == ASSET_READ_OK; }
};

struct AssetReader {
    File file{};
    // The header of the open file, valid while is_open().
    AssetHeader header{};

    AssetReader() = default;
    AssetReader(const AssetReader&) = delete;
    AssetReader& operator=(const AssetReader&) = delete;

    // Opens `path` and validates its header: magic, format version and that
    // the file on disk is exactly `file_size` bytes. False (and a message on
    // stderr) otherwise; the reader stays closed. Whatever was open before
    // is closed first.
    bool open(const char* path);
    void close();
    bool is_open() const { return this->file.is_open(); }

    // Whether the open file is the one `view` describes: same guid, size and
    // content hash. A view that fails this was taken before the file changed.
    bool matches(const AssetView& view) const;

    // Reads the prelude of the open file into a fresh PAYLOAD_ALIGNMENT
    // aligned buffer from `allocator` and parses it. The buffer is the view's
    // `data` (`size` bytes); release it with ASSET_FILE::free_prelude. A view
    // that is not ok() owns nothing; its parse_error says why (ASSET_FILE_ERROR
    // for a closed reader or a short read).
    AssetView read_prelude(BaseAllocator* allocator);

    // Reads `chunk`'s payload into `out`, which must hold `chunk.size` bytes.
    // `chunk` is an entry of a view that matches() this file. False (and a
    // message) if the entry does not fit the file or the read fails; `out`
    // is then unspecified. A size 0 chunk reads nothing and succeeds.
    bool read_chunk(const ChunkEntry& chunk, void* out);

    // Reads `size` bytes starting `offset` bytes into `chunk`'s payload into
    // `out`: a partial read for a chunk whose start says where the rest
    // lies (a SOURCE chunk's prefix, source_chunk.hpp). The range must lie
    // inside the chunk and the chunk inside the file; false (and a message)
    // otherwise or on a short read. A `size` of 0 reads nothing and succeeds.
    bool read_bytes(const ChunkEntry& chunk, u64 offset, void* out, usz size);

    // Reads `chunk`'s payload into a fresh PAYLOAD_ALIGNMENT aligned buffer
    // of `chunk.size` bytes from `allocator`; the caller frees it. nullptr on
    // failure, and also for a size 0 chunk, which has nothing to read.
    void* read_chunk(const ChunkEntry& chunk, BaseAllocator* allocator);

    // Reads the first chunk tagged `tag` in `view`, which must match() this
    // file, into a fresh aligned buffer. The result carries the entry read;
    // read_error says why nothing was read otherwise.
    ReadChunk read_chunk(const AssetView& view, u32 tag, BaseAllocator* allocator);
};

namespace ASSET_FILE {

// Opens the file at `path`, reads only as much of it as the prelude needs,
// and parses it: an AssetReader open + read_prelude + close. This is what an
// asset scan calls per file; to read payloads afterwards, open the file with
// an AssetReader. The view's `data` is the buffer, released with free_prelude.
AssetView read_prelude(const char* path, BaseAllocator* allocator);

// Frees the buffer a read_prelude view points into and resets the view.
void free_prelude(AssetView* view, BaseAllocator* allocator);

} // namespace ASSET_FILE
