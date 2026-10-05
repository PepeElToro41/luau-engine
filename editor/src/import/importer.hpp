#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/asset/asset_writer.hpp"
#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"

#include <string>

// What every importer shares: reading the source file, hashing it for the
// header's content_hash, minting a GUID for a new asset, the common editor
// chunks and writing the finished file. An importer (obj_importer.hpp,
// image_importer.hpp) decodes one source format into the core's
// MeshSource / TextureSource, hands that to the matching asset writer and
// uses these helpers for everything around it.
namespace IMPORT {

// The whole file at `path` in a buffer from `allocator` (caller frees), or
// nullptr with `error` set. Empty files are an error: nothing imports from
// zero bytes.
u8* read_file(const char* path, BaseAllocator* allocator, usz* out_size, std::string* error);

// 64-bit FNV-1a over the bytes: what goes in AssetHeader::content_hash so a
// re-import of the same source with the same settings is recognisable.
u64 fnv1a(const void* data, usz size);

// A fresh random GUID, never null.
AssetGuid random_guid();

// Appends the NAME, IMPS and SRC chunks every asset carries. Their payload
// layouts are not defined yet (docs/asset_format.md, open items), so they
// are written as EDITOR_ONLY placeholders with size 0 and version 0, which
// gives the chunk table its final shape today.
void add_editor_chunks(AssetWriter& writer);

// Writes `writer` to `path`, replacing any existing file. False with `error`
// set when the writer refuses (null guid) or the file cannot be written.
bool write_asset(const AssetWriter& writer, const char* path, std::string* error);

} // namespace IMPORT
