#pragma once

#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <bit>

// The `.lunaasset` container: one binary file per asset holding its identity,
// dependencies, import settings, original source and compiled data as a table
// of chunks. This header is the byte layout plus a validating view over the
// *prelude* (header, dependency table, chunk table: AssetView) and a writer
// (AssetWriter); it knows nothing about what the chunks contain.
// asset_reader.hpp reads chunk payloads out of the file on demand, and
// docs/asset_format.md is the full specification.
//
//     u8* prelude = ASSET_FILE::read_prelude("wall.lunaasset", allocator, &size);
//     AssetView view;
//     if (view.parse(prelude, size) == ASSET_PARSE_OK) {
//         view.header->type, view.dependencies[i], view.find_chunk(CHUNK_TAG::PIXELS)->size ...
//     }
//
// A view is all the editor's asset browser and references ever hold: the
// prelude is small and at the start of the file. Payloads are read later,
// one chunk at a time, by seeking with the chunk table (see AssetReader).
// The file is little-endian and every payload is 64-byte aligned, so a
// payload read into an aligned buffer is used in place; nothing is parsed.

static_assert(std::endian::native == std::endian::little, "the asset format is little-endian; big-endian targets are not supported");

namespace ASSET_FILE {

constexpr const char* EXTENSION = ".lunaasset";

// Four ASCII characters packed so the bytes in the file read as the name in a
// hex dump. Pad shorter names with spaces: fourcc("SRC ").
constexpr u32 fourcc(const char (&name)[5]) {
    return static_cast<u32>(static_cast<u8>(name[0])) | static_cast<u32>(static_cast<u8>(name[1])) << 8 |
           static_cast<u32>(static_cast<u8>(name[2])) << 16 | static_cast<u32>(static_cast<u8>(name[3])) << 24;
}

constexpr u32 MAGIC = fourcc("LUAS");
constexpr u32 FORMAT_VERSION = 1;
// Every chunk payload starts at a multiple of this, from the file start.
constexpr usz PAYLOAD_ALIGNMENT = 64;

constexpr usz align_up(const usz value, const usz alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace ASSET_FILE

namespace ASSET_TYPE {

constexpr u32 TEXTURE = ASSET_FILE::fourcc("TEX2");
constexpr u32 MESH = ASSET_FILE::fourcc("MESH");
// Reserved, nothing writes these yet.
constexpr u32 MATERIAL = ASSET_FILE::fourcc("MATL");
constexpr u32 SCENE = ASSET_FILE::fourcc("SCNE");
constexpr u32 SHADER = ASSET_FILE::fourcc("SHDR");

} // namespace ASSET_TYPE

namespace ASSET_FLAG {

// Editor-only chunks were stripped. The runtime refuses files without it.
constexpr u32 COOKED = 1u << 0;

} // namespace ASSET_FLAG

namespace CHUNK_FLAG {

// Stripped by cooking; the runtime never reads these chunks.
constexpr u32 EDITOR_ONLY = 1u << 0;
// Reserved, unused in format version 1.
constexpr u32 COMPRESSED = 1u << 1;

} // namespace CHUNK_FLAG

// Chunk tags. Payload layouts for all of these are still to be defined;
// importers currently write them with size 0 and version 0 so the chunk table
// of files produced now already has the right shape.
namespace CHUNK_TAG {

// Common to every asset type (all EDITOR_ONLY).
constexpr u32 NAME = ASSET_FILE::fourcc("NAME");            // display name and user tags
constexpr u32 IMPORT_SETTINGS = ASSET_FILE::fourcc("IMPS"); // per-type import settings struct
constexpr u32 SOURCE = ASSET_FILE::fourcc("SRC ");          // original filename + imported bytes

// Texture (ASSET_TYPE::TEXTURE).
constexpr u32 TEXTURE = ASSET_FILE::fourcc("TEX2"); // dimensions, format, mip table
constexpr u32 PIXELS = ASSET_FILE::fourcc("PIXL");  // mip data

// Mesh (ASSET_TYPE::MESH).
constexpr u32 MESH = ASSET_FILE::fourcc("MESH");     // vertex layout, submesh table
constexpr u32 VERTICES = ASSET_FILE::fourcc("VERT"); // one chunk per vertex stream
constexpr u32 INDICES = ASSET_FILE::fourcc("INDX");  // index data
constexpr u32 BOUNDS = ASSET_FILE::fourcc("BBOX");   // axis-aligned bounds

} // namespace CHUNK_TAG

// --- On-disk structures ------------------------------------------------------
// Written and read verbatim: sizes and offsets are asserted below and must not
// change without bumping ASSET_FILE::FORMAT_VERSION.

struct AssetGuid {
    u64 lo = 0;
    u64 hi = 0;

    // The all-zero GUID means "no asset" and never appears in a valid file.
    bool is_null() const { return this->lo == 0 && this->hi == 0; }

    friend bool operator==(const AssetGuid& a, const AssetGuid& b) { return a.lo == b.lo && a.hi == b.hi; }
    friend bool operator!=(const AssetGuid& a, const AssetGuid& b) { return !(a == b); }
};

struct AssetHeader {
    u32 magic = ASSET_FILE::MAGIC;
    u32 format_version = ASSET_FILE::FORMAT_VERSION;
    u32 type = 0;  // ASSET_TYPE::*
    u32 flags = 0; // ASSET_FLAG::*
    AssetGuid guid;
    // Hash of the SOURCE and IMPORT_SETTINGS payloads at import time; a
    // mismatch means the compiled chunks are stale.
    u64 content_hash = 0;
    u32 dependency_count = 0;
    u32 chunk_count = 0;
    u64 file_size = 0;
    u64 reserved = 0;
};

struct ChunkEntry {
    u32 tag = 0;     // CHUNK_TAG::*
    u32 version = 0; // layout version of the payload, owned by whoever writes `tag`
    u32 flags = 0;   // CHUNK_FLAG::*
    u32 reserved = 0;
    u64 offset = 0; // from the file start; multiple of PAYLOAD_ALIGNMENT
    u64 size = 0;   // may be 0

    bool is_editor_only() const { return (this->flags & CHUNK_FLAG::EDITOR_ONLY) != 0; }
};

static_assert(sizeof(AssetGuid) == 16, "AssetGuid must be 16 bytes on disk");
static_assert(sizeof(AssetHeader) == 64, "AssetHeader must be 64 bytes on disk");
static_assert(sizeof(ChunkEntry) == 32, "ChunkEntry must be 32 bytes on disk");
static_assert(alignof(AssetHeader) <= 8 && alignof(ChunkEntry) <= 8, "tables are placed at 8-byte aligned offsets");

namespace ASSET_FILE {

// Bytes from the file start to the first payload, for a file with these
// counts: header, dependency table and chunk table, rounded up to
// PAYLOAD_ALIGNMENT. A reader that has this many bytes can answer everything
// except payload contents.
constexpr usz prelude_size(const u32 dependency_count, const u32 chunk_count) {
    return sizeof(AssetHeader) + sizeof(AssetGuid) * dependency_count + sizeof(ChunkEntry) * chunk_count;
}

constexpr usz payload_start(const u32 dependency_count, const u32 chunk_count) {
    return align_up(prelude_size(dependency_count, chunk_count), PAYLOAD_ALIGNMENT);
}

} // namespace ASSET_FILE

// --- Reading ------------------------------------------------------------------

enum AssetParseError {
    ASSET_PARSE_OK = 0,
    ASSET_PARSE_TOO_SMALL,           // the buffer does not hold the whole prelude
    ASSET_PARSE_BAD_MAGIC,           // not a .lunaasset file
    ASSET_PARSE_UNSUPPORTED_VERSION, // format_version is not one this build reads
    ASSET_PARSE_BAD_HEADER,          // null guid, file_size below the prelude, or buffer longer than file_size
    ASSET_PARSE_BAD_CHUNK,           // a chunk entry is misaligned, out of bounds or overlaps another
};

namespace ASSET_FILE {

const char* parse_error_name(AssetParseError error);

}

// Non-owning view over a buffer that holds the prelude of a .lunaasset file:
// the header, the dependency table and the chunk table. parse() validates
// the layout and sets the pointers; they stay valid as long as the buffer
// does. The view holds no memory of its own and never touches payloads: it
// knows where every chunk is (offset, size, tag, version, flags), and an
// AssetReader fetches the bytes.
struct AssetView {
    const u8* data = nullptr;
    usz size = 0;

    const AssetHeader* header = nullptr;
    const AssetGuid* dependencies = nullptr; // header->dependency_count entries
    const ChunkEntry* chunks = nullptr;      // header->chunk_count entries

    // Validates `size` bytes at `data` as described in docs/asset_format.md.
    // The buffer is what read_prelude returns; a longer prefix of the file,
    // or the whole file, parses the same way. `data` must be 8-byte aligned,
    // which every allocator guarantees. On any error the view is reset to
    // empty and nothing else is touched.
    AssetParseError parse(const void* data, usz size);
    void reset();

    bool is_parsed() const { return this->header != nullptr; }
    bool is_cooked() const { return this->header != nullptr && (this->header->flags & ASSET_FLAG::COOKED) != 0; }

    usz dependency_count() const { return this->header != nullptr ? this->header->dependency_count : 0; }
    usz chunk_count() const { return this->header != nullptr ? this->header->chunk_count : 0; }

    // The first chunk with `tag`, or nullptr. Tags may repeat; use
    // find_chunk(tag, after) to walk them: `after` is the previous match.
    const ChunkEntry* find_chunk(u32 tag) const;
    const ChunkEntry* find_chunk(u32 tag, const ChunkEntry* after) const;

    // Index of `guid` in the dependency table, or dependency_count() if absent.
    usz find_dependency(const AssetGuid& guid) const;

    // Whether any chunk is EDITOR_ONLY; false for a correctly cooked file.
    bool has_editor_chunks() const;
};

// --- Writing ------------------------------------------------------------------

// Collects the parts of a .lunaasset file and emits them in the on-disk layout.
// Payload bytes are copied when added, so callers need not keep them alive.
// Chunks are written in the order they were added. Call free() when done.
struct AssetWriter {
    AssetWriter();
    explicit AssetWriter(BaseAllocator* allocator);

    AssetWriter(const AssetWriter&) = delete;
    AssetWriter& operator=(const AssetWriter&) = delete;

    u32 type = 0;
    u32 flags = 0;
    AssetGuid guid;
    u64 content_hash = 0;

    // Adds `guid` to the dependency table and returns its index; a GUID already
    // present returns its existing index. Null GUIDs are rejected (returns
    // ~0u) since a valid file never holds one.
    u32 add_dependency(const AssetGuid& guid);

    // Appends a chunk with a copy of `size` bytes at `payload`. `payload` may
    // be nullptr when `size` is 0. Returns the chunk's index.
    u32 add_chunk(u32 tag, u32 version, u32 flags, const void* payload, usz size);

    usz dependency_count() const { return this->dependencies.count; }
    usz chunk_count() const { return this->chunks.count; }

    // Size in bytes of the file write() produces.
    usz file_size() const;

    // Writes the complete file into `out`, which must hold file_size() bytes.
    // Returns false and writes nothing if `guid` is null.
    bool write(void* out) const;

    // Allocates file_size() bytes on `allocator`, writes the file into them
    // and returns the buffer (caller frees it). nullptr on failure.
    u8* write(BaseAllocator* allocator, usz* out_size) const;

    // Forgets every dependency and chunk, keeping the header fields.
    void clear();
    // Releases the storage.
    void free();

private:
    DynamicArray<AssetGuid> dependencies;
    DynamicArray<ChunkEntry> chunks; // `offset` is relative to the body start
    DynamicArray<u8> body;           // payloads, each at a PAYLOAD_ALIGNMENT-relative offset
};

// --- Files ----------------------------------------------------------------------

namespace ASSET_FILE {

// Reads only as much of the file at `path` as the prelude needs: the header
// first, then the tables. The returned buffer parses with AssetView. nullptr
// (and a message on stderr) if the file cannot be read or does not start
// with a valid header. This is what an asset scan calls per file; to read
// payloads afterwards, open the file with an AssetReader (asset_reader.hpp).
u8* read_prelude(const char* path, BaseAllocator* allocator, usz* out_size);

// Writes `size` bytes to `path`, replacing any existing file. False (and a
// message on stderr) on failure.
bool write_file(const char* path, const void* data, usz size);

} // namespace ASSET_FILE
