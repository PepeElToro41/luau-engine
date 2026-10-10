#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/asset/asset_writer.hpp"
#include "engine/defines.hpp"

// The SRC chunk (CHUNK_TYPE::SOURCE): the bytes an asset was imported from,
// kept inside the .lunaasset so the editor can re-import it with other
// settings or hand the original back out, preceded by the original file
// name so the extension (which decides the importer) survives. Editor-only;
// cooking strips it. Keeping the source is the importer's choice: an asset
// imported without it carries the chunk as a size 0, version 0 placeholder,
// which has_source() reports as "nothing kept".
//
// Payload layout, version SOURCE_CHUNK::VERSION:
//
//     offset 0    SourceChunkHeader (16 bytes): name_size, data_size
//     offset 16   the file name, name_size bytes of UTF-8, no terminator
//     ...         zero padding up to SOURCE_CHUNK::data_offset(name_size)
//     data        data_size bytes, the source file verbatim
//
// The prefix (header + name) fits in SOURCE_CHUNK::PREFIX_SIZE bytes, so a
// reader that wants only the name and where the data lies reads that much
// (AssetReader::read_bytes) and parses it with SourceChunkView; the blob
// itself can be several megabytes and is read only to re-import or export.
//
//     SOURCE_CHUNK::add_chunk(writer, "rock.obj", bytes, size);   // importing
//     ...
//     SourceChunkView source = SourceChunkView::parse(*entry, prefix, prefix_size);
//     if (source.is_ok()) { entry->offset + source.data_offset, source.data_size ... }

struct SourceChunkHeader {
    u32 name_size = 0; // bytes of the file name after the header, 1..MAX_NAME_SIZE
    u32 reserved = 0;  // zero
    u64 data_size = 0; // bytes of the source file at data_offset(name_size)
};
static_assert(sizeof(SourceChunkHeader) == 16, "SourceChunkHeader is an on-disk layout: 16 bytes");

namespace SOURCE_CHUNK {

constexpr u32 VERSION = 1;
// Longest file name stored, in bytes. Chosen so the prefix is exactly 256 bytes.
constexpr usz MAX_NAME_SIZE = 240;
// The data starts at a multiple of this from the payload start.
constexpr usz DATA_ALIGNMENT = 16;

// Where the data lies in the payload for a name of `name_size` bytes.
constexpr usz data_offset(const usz name_size) {
    return ASSET_FILE::align_up(sizeof(SourceChunkHeader) + name_size, DATA_ALIGNMENT);
}
// The whole payload for a name and a blob of these sizes.
constexpr usz payload_size(const usz name_size, const usz data_size) {
    return data_offset(name_size) + data_size;
}
// Bytes that are sure to hold the header and the name of any valid payload:
// what to read to learn the name and the data's position.
constexpr usz PREFIX_SIZE = data_offset(MAX_NAME_SIZE);
static_assert(PREFIX_SIZE == 256, "the prefix read is one 256-byte block");

// Whether `entry` (a SOURCE chunk entry, or nullptr) holds a kept source:
// version VERSION with a payload. The placeholder (size 0) and unknown
// versions report false.
bool has_source(const ChunkEntry* entry);
// Same for the first SOURCE chunk of `view`; false when there is none.
bool has_source(const AssetView& view);

// Appends a SOURCE chunk (EDITOR_ONLY, VERSION) holding `name` (`name_size`
// bytes, a bare file name) and `data_size` bytes of `data`. Returns the
// chunk's index, or ~0u without adding anything when the name is empty or
// longer than MAX_NAME_SIZE, or when `data` is null with a nonzero size.
u32 add_chunk(AssetWriter& writer, const char* name, usz name_size, const void* data, usz data_size);
// The same with a NUL-terminated `name`.
u32 add_chunk(AssetWriter& writer, const char* name, const void* data, usz data_size);
// Appends the placeholder written when the source is not kept: size 0,
// version 0, EDITOR_ONLY. Returns the chunk's index.
u32 add_placeholder(AssetWriter& writer);

} // namespace SOURCE_CHUNK

enum SourceParseError {
    SOURCE_PARSE_OK = 0,
    SOURCE_PARSE_NO_SOURCE,           // the entry is the placeholder (size 0): nothing was kept
    SOURCE_PARSE_UNSUPPORTED_VERSION, // the entry's version is not one this build reads
    SOURCE_PARSE_TOO_SMALL,           // the buffer does not hold the header and the name
    SOURCE_PARSE_BAD_NAME,            // name_size is 0 or above MAX_NAME_SIZE
    SOURCE_PARSE_BAD_SIZE,            // the entry's size is not payload_size(name_size, data_size)
};

namespace SOURCE_CHUNK {

const char* parse_error_name(SourceParseError error);

}

// Validating view over a SOURCE payload, or over its prefix. Points into the
// buffer given to parse(); `data` is set only when the buffer holds the
// whole payload, otherwise the caller reads `data_size` bytes at
// `data_offset` into the chunk itself.
struct SourceChunkView {
    SourceParseError parse_error = SOURCE_PARSE_OK;

    const SourceChunkHeader* header = nullptr;
    const char* name = nullptr; // name_size bytes, not terminated
    usz name_size = 0;
    u64 data_offset = 0;        // from the payload start
    u64 data_size = 0;
    const u8* data = nullptr;   // nullptr when the buffer stops before the data's end

    SourceChunkView() = default;
    explicit SourceChunkView(const SourceParseError error) : parse_error(error) {}

    // Validates `size` bytes at `payload`, the start of the payload of
    // `entry` (its version and size are checked too). `size` may be anything
    // from the prefix (header + name) up to the whole payload; PREFIX_SIZE
    // bytes, or the entry's size when smaller, is always enough.
    static SourceChunkView parse(const ChunkEntry& entry, const void* payload, usz size);

    bool is_ok() const { return this->parse_error == SOURCE_PARSE_OK && this->header != nullptr; }
    bool has_data() const { return this->data != nullptr; }
};
