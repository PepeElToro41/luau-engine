#pragma once

#include "engine/asset/asset_file.hpp"
#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"

#include <cstdio>

// Reads chunk payloads out of a .lunaasset file on demand. An AssetView
// (from ASSET_FILE::read_prelude, typically scanned long before anything is
// loaded) says where every chunk is; the reader opens the file and seeks
// straight to the chunks asked for, so loading a mesh's bounds costs one
// 48-byte read and nothing else in the file is touched.
//
//     AssetReader reader;
//     if (reader.open("rock.lunaasset") && reader.matches(view)) {
//         const ChunkEntry* entry = nullptr;
//         u8* mesh = reader.read_chunk(view, CHUNK_TAG::MESH, allocator, &entry);
//         ...
//         allocator->free(mesh);
//     }
//     reader.close();
//
// open() reads the header back and checks it against the file on disk, and
// matches() checks it against the view, so a view from a stale scan (the
// file was re-imported since) is caught before any chunk is trusted; the
// caller then re-reads the prelude. The reader holds a FILE* and nothing
// else; close() when done.
struct AssetReader {
    AssetReader() = default;
    AssetReader(const AssetReader&) = delete;
    AssetReader& operator=(const AssetReader&) = delete;

    // The header of the open file, valid while is_open().
    AssetHeader header;

    // Opens `path` and validates its header: magic, format version and that
    // the file on disk is exactly `file_size` bytes. False (and a message on
    // stderr) otherwise; the reader stays closed.
    bool open(const char* path);
    void close();
    bool is_open() const { return this->file != nullptr; }

    // Whether the open file is the one `view` describes: same guid, size and
    // content hash. A view that fails this was taken before the file changed.
    bool matches(const AssetView& view) const;

    // Reads the prelude of the open file into a fresh PAYLOAD_ALIGNMENT
    // aligned buffer from `allocator`, for parsing with AssetView. nullptr
    // (and a message) on a short read. This is how a view is refreshed.
    u8* read_prelude(BaseAllocator* allocator, usz* out_size);

    // Reads `chunk`'s payload into `out`, which must hold `chunk.size` bytes.
    // `chunk` is an entry of a view that matches() this file. False (and a
    // message) if the entry does not fit the file or the read fails; `out`
    // is then unspecified. A size 0 chunk reads nothing and succeeds.
    bool read_chunk(const ChunkEntry& chunk, void* out);

    // Reads `chunk`'s payload into a fresh PAYLOAD_ALIGNMENT aligned buffer
    // of `chunk.size` bytes from `allocator`; the caller frees it. nullptr on
    // failure, and also for a size 0 chunk, which has nothing to read.
    u8* read_chunk(const ChunkEntry& chunk, BaseAllocator* allocator);

    // Reads the first chunk tagged `tag` in `view`, which must match() this
    // file. `out_chunk`, when given, receives the entry read (nullptr when
    // absent) so the caller knows the size and version. nullptr when the
    // view does not match, the tag is absent, the chunk is empty or the
    // read fails.
    u8* read_chunk(const AssetView& view, u32 tag, BaseAllocator* allocator, const ChunkEntry** out_chunk = nullptr);

private:
    FILE* file = nullptr;
};
