#pragma once

#include "engine/defines.hpp"

#include <bit>

// The `.lunaasset` container: one binary file per asset holding its identity,
// dependencies, import settings, original source and compiled data as a table
// of chunks. This header is the byte layout plus a validating view over the
// *prelude* (header, dependency table, chunk table: AssetView); it knows
// nothing about what the chunks contain. asset_writer.hpp emits files,
// asset_reader.hpp reads chunk payloads out of them on demand, and
// docs/asset_format.md is the full specification.
//
//     AssetView view = AssetView::parse(prelude, size);
//     if (view.is_ok()) {
//         view.header->type, view.dependencies[i], view.find_chunk(CHUNK_TYPE::PIXELS)->size ...
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

// Chunk types (the `tag` of a ChunkEntry). The payload layouts of the texture
// and mesh chunks are in asset_types/; the common editor chunks are still to
// be defined and are written with size 0 and version 0 for now.
namespace CHUNK_TYPE {

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

} // namespace CHUNK_TYPE

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

// --- On-disk structures ------------------------------------------------------
// Written and read verbatim: sizes are asserted below and must not change
// without bumping ASSET_FILE::FORMAT_VERSION.

struct AssetGuid {
    u64 lo = 0;
    u64 hi = 0;

    // The all-zero GUID means "no asset" and never appears in a valid file.
    bool is_null() const { return this->lo == 0 && this->hi == 0; }

    friend bool operator==(const AssetGuid& a, const AssetGuid& b) { return a.lo == b.lo && a.hi == b.hi; }
    friend bool operator!=(const AssetGuid& a, const AssetGuid& b) { return !(a == b); }
};

// Hash functor for keying a HashMap by GUID: HashMap<AssetGuid, V, AssetGuidHash>.
struct AssetGuidHash {
    usz operator()(const AssetGuid& guid) const {
        // GUIDs are random bits; folding the halves is enough and the map
        // finalizes the result itself.
        return static_cast<usz>(guid.lo ^ (guid.hi * 0x9e3779b97f4a7c15ull));
    }
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
    u32 tag = 0;     // CHUNK_TYPE::*
    u32 version = 0; // layout version of the payload, owned by whoever writes `tag`
    u32 flags = 0;   // CHUNK_FLAG::*
    u32 reserved = 0;
    u64 offset = 0; // from the file start; multiple of PAYLOAD_ALIGNMENT
    u64 size = 0;   // may be 0

    bool editor_only() const { return (this->flags & CHUNK_FLAG::EDITOR_ONLY) != 0; }
};

static_assert(sizeof(AssetGuid) == 16, "AssetGuid must be 16 bytes on disk");
static_assert(sizeof(AssetHeader) == 64, "AssetHeader must be 64 bytes on disk");
static_assert(sizeof(ChunkEntry) == 32, "ChunkEntry must be 32 bytes on disk");
static_assert(alignof(AssetHeader) <= 8 && alignof(ChunkEntry) <= 8, "tables are placed at 8-byte aligned offsets");

namespace ASSET_FILE {

// Bytes of the header, dependency table and chunk table for these counts.
// A reader that has this many bytes can answer everything except payload
// contents.
constexpr usz prelude_size(const usz dependency_count, const usz chunk_count) {
    return sizeof(AssetHeader) + sizeof(AssetGuid) * dependency_count + sizeof(ChunkEntry) * chunk_count;
}
constexpr usz prelude_size(const AssetHeader& header) {
    return prelude_size(header.dependency_count, header.chunk_count);
}

// Where the first payload starts: the prelude rounded up to PAYLOAD_ALIGNMENT.
constexpr usz payload_start(const usz dependency_count, const usz chunk_count) {
    return align_up(prelude_size(dependency_count, chunk_count), PAYLOAD_ALIGNMENT);
}
constexpr usz payload_start(const AssetHeader& header) {
    return align_up(prelude_size(header), PAYLOAD_ALIGNMENT);
}

} // namespace ASSET_FILE

// --- Reading ------------------------------------------------------------------

enum AssetParseError {
    ASSET_PARSE_OK = 0,
    ASSET_FILE_ERROR,                // the file could not be opened or read; set by AssetReader, a view owns no file
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
// the layout and returns a view pointing into the buffer; the pointers stay
// valid as long as the buffer does. The view holds no memory of its own and
// never touches payloads: it knows where every chunk is (offset, size, tag,
// version, flags), and an AssetReader fetches the bytes.
struct AssetView {
    // Why parse failed; ASSET_PARSE_OK for a good view and for a default one
    // (which has no header either, so test is_ok(), not this).
    AssetParseError parse_error = ASSET_PARSE_OK;

    const u8* data = nullptr;
    usz size = 0;

    const AssetHeader* header = nullptr;
    const AssetGuid* dependencies = nullptr; // header->dependency_count entries
    const ChunkEntry* chunks = nullptr;      // header->chunk_count entries

    AssetView() = default;
    explicit AssetView(const AssetParseError error) : parse_error(error) {}

    // Validates `size` bytes at `data` as described in docs/asset_format.md
    // and returns the view. The buffer is what AssetReader::read_prelude
    // fills; a longer prefix of the file, or the whole file, parses the same
    // way. `data` must be 8-byte aligned, which every allocator guarantees.
    // On any error the view is empty with `parse_error` set.
    static AssetView parse(const void* data, usz size);

    // The checks that need only the header: magic, version, non-null guid
    // and a `file_size` that holds the tables. parse() runs these first and
    // AssetReader::open runs them before trusting a file.
    static AssetParseError parse_header(const AssetHeader& header);

    bool is_ok() const { return this->parse_error == ASSET_PARSE_OK && this->header != nullptr; }
    bool is_cooked() const { return this->header != nullptr && (this->header->flags & ASSET_FLAG::COOKED) != 0; }

    usz dependency_count() const { return this->header != nullptr ? this->header->dependency_count : 0; }
    usz chunk_count() const { return this->header != nullptr ? this->header->chunk_count : 0; }

    // The first chunk with `tag`, or nullptr. Tags may repeat (one VERT per
    // vertex stream); use find_chunk(tag, after) to walk them: `after` is the
    // previous match.
    const ChunkEntry* find_chunk(u32 tag) const;
    const ChunkEntry* find_chunk(u32 tag, const ChunkEntry* after) const;

    // Index of `guid` in the dependency table, or dependency_count() if absent.
    usz find_dependency(const AssetGuid& guid) const;

    // Whether any chunk is EDITOR_ONLY; false for a correctly cooked file.
    bool has_editor_chunks() const;
};
