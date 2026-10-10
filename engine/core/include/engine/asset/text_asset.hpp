#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"

// Text assets: hand-authored assets (a `.material`, later a `.scene`) are
// UTF-8 text files, not .lunaasset containers, so they diff and merge in
// version control. They still go through the AssetResourceProvider like
// every other asset: this header turns a text file into the prelude the
// provider keys on (an AssetHeader with ASSET_FLAG::TEXT and one TEXT chunk
// covering the file's bytes, built in memory, never on disk), and the
// provider reads the file itself when the chunk is asked for. The payload
// of a text asset is its text; asset_types/material_asset.hpp parses it.
//
//     AssetView view = TEXT_ASSET::read_prelude("materials/rock.material", allocator);
//     provider->add(view, path);                           // like a .lunaasset prelude
//     ASSET_FILE::free_prelude(&view, allocator);
//     ...
//     AssetResource* rock = provider->get(guid);
//     const ChunkEntry* entry = nullptr;
//     const u8* text = rock->find_payload(CHUNK_TYPE::TEXT, &entry);  // entry->size bytes, not terminated
//
// The syntax every text asset shares is a flat key = value format:
//
//     # comment
//     guid = e26f6a98c55cbe22179a52dd06b33f96    (top-level keys identify the asset)
//     shader = unlit
//
//     [params]                                     (a section, until the next one)
//     color = 1 0.5 0.15 1
//     name = "quoted when it holds # or spaces"
//
// Lines are trimmed; blank lines and `#` comments are skipped; a value runs
// to the end of the line or a `#`, unless it is double quoted. What the keys
// and sections mean belongs to the asset type; TextAssetCursor only walks
// them. Every text asset has a top-level `guid` (32 hex digits, hi then lo),
// which is its identity exactly as a .lunaasset header's, and its type is
// its extension (`.material` is ASSET_TYPE::MATERIAL). The header's
// `content_hash` is the hash of the text and `file_size` is what the
// virtual file would be, so the provider's staleness check works unchanged.

namespace TEXT_ASSET {

// Version of the TEXT chunk: the file's bytes, UTF-8, no terminator.
constexpr u32 VERSION = 1;
// A text asset is small; anything larger is refused as not one.
constexpr usz MAX_FILE_SIZE = 16u << 20;
// Characters of a GUID in text form, plus the terminator for format_guid.
constexpr usz GUID_TEXT_LENGTH = 32;
constexpr usz GUID_TEXT_CAPACITY = GUID_TEXT_LENGTH + 1;

constexpr const char* MATERIAL_EXTENSION = ".material";

// The asset type a text file's extension names (`.material` ->
// ASSET_TYPE::MATERIAL), or 0 when `path` is not a text asset's. Case
// insensitive.
u32 type_of_path(const char* path);
// The extension of a text asset type, with the dot; nullptr for a type that
// is not a text asset.
const char* extension_of_type(u32 type);

// --- GUIDs ------------------------------------------------------------------

// `hi` then `lo` as 32 lowercase hex digits, the way the engine prints
// GUIDs everywhere. `out` holds GUID_TEXT_CAPACITY characters.
void format_guid(const AssetGuid& guid, char* out);
// Parses 32 hex digits (either case; `-` between digits is ignored). False,
// with `out` untouched, for anything else or for the null GUID.
bool parse_guid(const char* text, usz length, AssetGuid* out);

} // namespace TEXT_ASSET

// --- The line syntax --------------------------------------------------------

// One `key = value` line as the cursor reports it. Pointers point into the
// text the cursor walks and are not terminated: use the lengths or the
// `_is` helpers.
struct TextAssetEntry {
    // The enclosing `[section]`, empty before the first one.
    const char* section = nullptr;
    usz section_length = 0;
    const char* key = nullptr;
    usz key_length = 0;
    // Trimmed, comment stripped, quotes removed.
    const char* value = nullptr;
    usz value_length = 0;
    // 1-based line of the key, for messages.
    u32 line = 0;

    bool section_is(const char* name) const;
    bool key_is(const char* name) const;
    bool value_is(const char* text) const;
    bool in_top_level() const { return this->section_length == 0; }
};

// Walks the `key = value` lines of a text in order, tracking sections.
//
//     TextAssetCursor cursor(text, size);
//     TextAssetEntry entry;
//     while (cursor.next(&entry)) { ... }
//     if (cursor.error != nullptr) { ... cursor.line ... }
struct TextAssetCursor {
    const char* text = nullptr;
    usz size = 0;
    usz offset = 0;
    // The line the cursor is on; after a failed next() the malformed line.
    u32 line = 0;
    const char* section = nullptr;
    usz section_length = 0;
    // Set when next() stops on a line that is neither blank, a comment, a
    // section nor `key = value`; nullptr at a clean end.
    const char* error = nullptr;

    TextAssetCursor() = default;
    TextAssetCursor(const char* text, usz size) : text(text), size(size) {}

    // The next entry, skipping sections into `section`. False at the end of
    // the text or on a malformed line (`error` tells which).
    bool next(TextAssetEntry* out);
};

namespace TEXT_ASSET {

// Splits a value on whitespace and commas into numbers (strtod syntax).
// `out` holds `capacity` of them; false when a token is not a number or
// there are more than `capacity`. An empty value is zero numbers.
bool parse_numbers(const char* value, usz length, f64* out, usz capacity, usz* out_count);
// The next whitespace separated word of a value starting at `*offset`;
// advances `*offset` past it. False when nothing is left.
bool next_word(const char* value, usz length, usz* offset, const char** out_word, usz* out_length);
// Whether a word equals a name, by length and contents.
bool word_is(const char* word, usz length, const char* name);
// Copies a span into a fixed buffer, terminated; false (and the copy
// truncated) when it does not fit.
bool copy_span(const char* text, usz length, char* out, usz capacity);

// --- Preludes ---------------------------------------------------------------

// The prelude a text asset of `type` and `guid` with these bytes gets: a
// header with ASSET_FLAG::TEXT | COOKED, no dependencies and one TEXT chunk
// of `size` bytes at payload_start, in a fresh PAYLOAD_ALIGNMENT aligned
// buffer from `allocator` (release with ASSET_FILE::free_prelude). A view
// that is not ok() owns nothing: ASSET_PARSE_BAD_HEADER for a null guid, a
// type that is not a text asset's or a text over MAX_FILE_SIZE.
AssetView build_prelude(u32 type, const AssetGuid& guid, const void* text, usz size, BaseAllocator* allocator);
// The same for the text of the file at `path`: the type from the extension
// and the guid from the text's top-level `guid` key. ASSET_PARSE_BAD_HEADER
// (with a message on stderr) when either is missing or malformed.
AssetView build_prelude(const char* path, const void* text, usz size, BaseAllocator* allocator);

// Reads the whole file at `path` into a fresh PAYLOAD_ALIGNMENT aligned
// buffer from `allocator` (the caller frees it). False when the file cannot
// be read or exceeds MAX_FILE_SIZE; an empty file is read as 0 bytes with
// a non-null buffer.
bool read_file(const char* path, BaseAllocator* allocator, u8** out_text, usz* out_size);

// Reads the file at `path` and builds its prelude: what ASSET_FILE::
// read_prelude is for a .lunaasset, for the asset scan and
// Engine::load_asset_file. The text is not kept; the provider reads it
// again on get(). ASSET_FILE_ERROR when the file cannot be read.
AssetView read_prelude(const char* path, BaseAllocator* allocator);

// Whether `size` bytes of text are the file `view` describes: same file
// size and content hash. What AssetReader::matches is for a .lunaasset.
bool matches(const AssetView& view, const void* text, usz size);

// The top-level `guid` of a text. False when there is none or it does not
// parse; `out_line` (when given) receives the line of a malformed one, 0
// when it is missing.
bool find_guid(const char* text, usz size, AssetGuid* out, u32* out_line = nullptr);

} // namespace TEXT_ASSET

namespace ASSET_FILE {

// Reads the prelude of whatever asset file `path` is: a text asset by its
// extension (TEXT_ASSET::read_prelude), a .lunaasset otherwise. This is
// the one call a scan or Engine::load_asset_file makes per file.
AssetView read_prelude_any(const char* path, BaseAllocator* allocator);

} // namespace ASSET_FILE
