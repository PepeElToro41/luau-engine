#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/asset/asset_writer.hpp"
#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"

#include <filesystem>
#include <string>

// What every importer shares: reading the source bytes, hashing them for the
// header's content_hash, minting a GUID for a new asset, the common editor
// chunks and writing the finished file. An importer (obj_importer.hpp,
// image_importer.hpp) decodes one source format into the core's
// MeshSource / TextureSource, hands that to the matching asset writer and
// uses these helpers for everything around it.
//
// The bytes an importer decodes come from an ImportSource: a plain file on
// disk, or a range of a .lunaasset holding the original in its SRC chunk
// (source_chunk.hpp), which is how a re-import works without the original
// file around. Either way the importer sees one buffer and a file name.

struct ImportSource {
    std::filesystem::path path; // the file holding the bytes: the source itself, or the .lunaasset
    u64 offset = 0;             // where the bytes start in `path`
    u64 size = 0;               // how many bytes; 0 means to the end of the file
    std::string name;           // the original file name (its extension picks the importer)
    // For bytes taken from an asset: that asset's GUID, so a re-import over
    // the same file keeps references valid. Null for a plain file.
    AssetGuid guid;

    // A plain file: the whole of `file`, named after it.
    static ImportSource file(const std::filesystem::path& file);

    // Whether the bytes sit inside a .lunaasset (found by IMPORT::find_source).
    bool is_embedded() const { return !this->guid.is_null(); }
    // `name`, or for an embedded source "asset.lunaasset (name)", for messages.
    std::string describe() const;
};

namespace IMPORT {

// The whole file at `path` in a buffer from `allocator` (caller frees), or
// nullptr with `error` set. Empty files are an error: nothing imports from
// zero bytes.
u8* read_file(const char* path, BaseAllocator* allocator, usz* out_size, std::string* error);

// The bytes of `source` (its range of its file) in a buffer from
// `allocator` (caller frees), or nullptr with `error` set. An empty range
// is an error as with read_file.
u8* read_source(const ImportSource& source, BaseAllocator* allocator, usz* out_size, std::string* error);

// Looks for a kept original in the .lunaasset at `asset`: reads its prelude
// and the SRC chunk's prefix and fills `out` with where the bytes are, the
// original name and the asset's GUID. False with `error` set when the file
// is not a readable asset, has no SRC chunk, or the chunk is the
// placeholder (nothing was kept) or unreadable.
bool find_source(const std::filesystem::path& asset, ImportSource* out, std::string* error);

// Writes the bytes of `source` to `destination`, replacing any existing
// file: how a kept original leaves the asset again. False with `error` set.
bool save_source(const ImportSource& source, const std::filesystem::path& destination, std::string* error);

// 64-bit FNV-1a over the bytes (HASH::fnv1a): what goes in
// AssetHeader::content_hash so a re-import of the same source with the same
// settings is recognisable.
u64 fnv1a(const void* data, usz size);

// A fresh random GUID, never null.
AssetGuid random_guid();

// Appends the NAME, IMPS and SRC chunks every asset carries. NAME and IMPS
// have no payload layout yet (docs/asset_format.md, open items) and are
// written as EDITOR_ONLY placeholders with size 0 and version 0. SRC holds
// `size` bytes of `bytes` under `source.name` when `keep_source` is set
// (SOURCE_CHUNK::add_chunk), otherwise the same placeholder. False with
// `error` set when the source cannot be stored (a name too long).
bool add_editor_chunks(AssetWriter& writer, const ImportSource& source, const void* bytes, usz size, bool keep_source,
                       std::string* error);

// Writes `writer` to `path`, replacing any existing file. False with `error`
// set when the writer refuses (null guid) or the file cannot be written.
bool write_asset(const AssetWriter& writer, const char* path, std::string* error);

} // namespace IMPORT
