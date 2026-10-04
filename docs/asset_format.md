# Asset file format

Every asset the engine knows about is one binary file with the `.lunaasset`
extension. Textures, meshes, materials and scenes all use the same container;
the asset type is a field in the header, not the extension. The file carries
everything the editor and the runtime need:

- the asset's identity (a GUID) and its dependencies on other assets,
- the import settings the editor used to build it,
- the original source bytes it was imported from (the `.png`, the `.gltf`),
  so re-importing with new settings never needs the original file and
  "export original" is a matter of writing one chunk back to disk,
- the compiled data the runtime actually reads.

There is no sidecar file and no separate cache: the compiled data lives next
to the settings and the source in the same file, and the editor rewrites the
compiled chunks in place when the settings change. Shipping is a *cook*: a copy
of the file without its editor-only chunks (see [Cooking](#cooking)).

The container is implemented in `engine/core/include/engine/asset/asset_file.hpp`
(`AssetView` reads, `AssetWriter` writes). Core has no external dependencies,
so the container stays a pure byte-layout description; importers and the
per-type payload code live in other modules.

## Design decisions

**One binary file, not text + sidecar.** Unity keeps a text `.meta` next to
the untouched source and compiles into a hidden cache; Unreal keeps one binary
package per asset with the source inside and compiles into a derived-data
cache. This format takes Unreal's single-file shape and additionally keeps the
compiled data in the same file, so there is exactly one loader and no cache to
invalidate. The cost is that assets are opaque to version control: `git diff`
cannot show a change to a material, and two people editing the same scene
cannot merge. A `dump` tool that prints the header, chunk table and the small
chunks as text, registered as a git `textconv` for `*.lunaasset`, recovers diffs;
merging stays manual.

**Identity travels with the file.** References between assets are GUIDs stored
in the header's dependency table, never paths. Moving or renaming a file in the
OS does not break references. The editor builds a path-to-GUID index by
scanning headers at startup and caches it. Two files with the same GUID (a file
copied in the OS) are resolved by the scan: the newer file gets a fresh GUID
and its header is rewritten.

**The header is readable without the payloads.** The asset browser and the
dependency scan read only the *prelude* (header, dependency table, chunk
table), which is at the start of the file and small. A reader given only the
prelude bytes can still answer type, GUID, dependencies and which chunks exist.

**Chunks version themselves.** The container's `format_version` only describes
the layout of the header and tables. Each chunk carries its own `version`, so a
texture importer can change the layout of its pixel chunk without touching
meshes, and only that importer needs a migration. Readers skip chunk tags they
do not know, so adding a chunk is never a breaking change.

**Payloads are plain data.** A chunk's payload is a plain struct or a flat
array: no pointers, no padding surprises, offsets or indices instead. Loading
is one read into an arena, a validation pass over the prelude, and pointers
into the buffer. Nothing is parsed.

**Little-endian only, 64-byte aligned.** The engine runs on little-endian
targets; the build fails on anything else. Every payload starts on a 64-byte
boundary so SIMD and cache-line sensitive data can be used in place.

**No compression in version 1.** The per-chunk `COMPRESSED` flag is reserved so
it can be added per chunk later without a container version bump.

## Layout

```
offset 0        AssetHeader          64 bytes, fixed
offset 64       AssetGuid[]          dependency_count x 16 bytes
                ChunkEntry[]         chunk_count x 32 bytes
                (padding to 64)
payload_start   chunk payloads       each 64-byte aligned, in chunk-table order
file_size       end                  no trailing padding
```

Everything up to `payload_start` is the *prelude*. Its size is
`64 + 16 * dependency_count + 32 * chunk_count`, and `payload_start` is that
rounded up to 64.

### AssetHeader (64 bytes)

| offset | type       | field              | meaning |
|-------:|------------|--------------------|---------|
| 0      | `u8[4]`    | `magic`            | `"LUAS"` |
| 4      | `u32`      | `format_version`   | container layout version, currently 1; readers reject other values |
| 8      | `u32`      | `type`             | asset type fourcc, see [Asset types](#asset-types) |
| 12     | `u32`      | `flags`            | `ASSET_FLAG::*` bits |
| 16     | `AssetGuid`| `guid`             | 128-bit identity, never zero in a valid file |
| 32     | `u64`      | `content_hash`     | hash of the `SRC ` and `IMPS` payloads at import time; tells whether the compiled chunks are stale |
| 40     | `u32`      | `dependency_count` | entries in the dependency table |
| 44     | `u32`      | `chunk_count`      | entries in the chunk table |
| 48     | `u64`      | `file_size`        | total size in bytes; a shorter buffer is a truncated file |
| 56     | `u64`      | `reserved`         | zero |

Asset flags:

| bit | name     | meaning |
|----:|----------|---------|
| 0   | `COOKED` | editor-only chunks were stripped; the runtime refuses files without it |

### AssetGuid (16 bytes)

Two `u64`, `lo` then `hi`. The all-zero GUID is "no asset" and never appears
in a header or a dependency table.

### Dependency table

Every asset this one references, by GUID, in no particular order but without
duplicates. In-chunk references to other assets are `u32` indices into this
table, never raw GUIDs, so a loader knows every dependency before it
understands a single payload and can queue them for loading first.

### ChunkEntry (32 bytes)

| offset | type  | field      | meaning |
|-------:|-------|------------|---------|
| 0      | `u32` | `tag`      | chunk fourcc, see [Chunk tags](#chunk-tags) |
| 4      | `u32` | `version`  | layout version of this chunk's payload, owned by the code that writes the tag |
| 8      | `u32` | `flags`    | `CHUNK_FLAG::*` bits |
| 12     | `u32` | `reserved` | zero |
| 16     | `u64` | `offset`   | payload start, from the beginning of the file; multiple of 64 |
| 24     | `u64` | `size`     | payload size in bytes; may be 0 |

Chunk flags:

| bit | name          | meaning |
|----:|---------------|---------|
| 0   | `EDITOR_ONLY` | stripped by cooking; the runtime never reads these |
| 1   | `COMPRESSED`  | reserved, not used in version 1 |

Chunks appear in the table in payload order: each entry's `offset` is at or
after the previous entry's `offset + size`, and the first is at
`payload_start`. A chunk with `size == 0` has an `offset` that is still a valid
aligned position, possibly equal to `file_size` when it is last. The same tag
may appear more than once (a mesh with several vertex streams); `find_chunk`
returns the first.

Readers validate every entry against `file_size`, not against the buffer they
were handed, so a prelude-only buffer validates the same way a complete file
does.

## Fourcc codes

Tags and types are four ASCII characters packed little-endian, so the bytes in
the file read as the name in a hex dump: `fourcc("TEX2")` is the bytes
`54 45 58 32`. Shorter names are padded with spaces (`"SRC "`).

### Asset types

| fourcc | name    | status |
|--------|---------|--------|
| `TEX2` | texture | type and chunk tags defined, payload layouts not yet |
| `MESH` | mesh    | type and chunk tags defined, payload layouts not yet |

Materials (`MATL`), scenes (`SCNE`) and shaders (`SHDR`) are planned and
reserve their fourcc; nothing writes them yet.

### Chunk tags

Common to every type:

| tag    | flags         | payload |
|--------|---------------|---------|
| `NAME` | `EDITOR_ONLY` | display name and user tags; what the asset browser shows. Layout not yet defined. |
| `IMPS` | `EDITOR_ONLY` | import settings, a plain struct per asset type (for textures: sRGB, mip generation, alpha bleeding, compression). Re-import reads `SRC `, applies this, rewrites the compiled chunks. Layout per type, not yet defined. |
| `SRC ` | `EDITOR_ONLY` | the original imported bytes, preceded by the original filename so the extension survives. Layout not yet defined. |

Texture (`TEX2`), payload layouts not yet defined:

| tag    | payload |
|--------|---------|
| `TEX2` | width, height, pixel format, mip count, per-mip offsets into `PIXL` |
| `PIXL` | mip data, tightly packed, each mip aligned |

Mesh (`MESH`), payload layouts not yet defined:

| tag    | payload |
|--------|---------|
| `MESH` | vertex layout, submesh table (index range and material slot per submesh) |
| `VERT` | vertex data; one chunk per stream |
| `INDX` | index data |
| `BBOX` | axis-aligned bounds |

Until the layouts exist, importers write these chunks with `size == 0` and
`version == 0` so that files produced now already have the right table and can
be re-imported in place once the payload code lands.

## Reading

`AssetView::parse(data, size)` validates a buffer and points into it. It
accepts either a complete file or any prefix long enough to hold the prelude:

1. `size >= sizeof(AssetHeader)`, magic matches, `format_version` is supported.
2. `file_size` is at least the prelude size, and `size <= file_size` (a buffer
   longer than the declared file is not this file).
3. `size` covers the prelude.
4. every chunk entry is 64-byte aligned, starts at or after `payload_start`,
   ends at or before `file_size`, and does not overlap the previous entry.

`is_complete()` tells whether the buffer holds the whole file. `chunk_data()`
returns `nullptr` for a chunk whose payload lies past the end of the buffer, so
code that only read the prelude cannot accidentally read payloads.

The runtime loader additionally rejects a file without `COOKED` set, so a
shipped build cannot carry sources by mistake. The editor accepts both.

## Writing

`AssetWriter` collects dependencies and chunks, then `write()` emits the file
in one pass: it computes the prelude size from the counts, places payloads in
the order they were added, 64-byte aligned, and fills `file_size`. The writer
never pads the end of the file. `ASSET_FILE::write_file` and `read_file` are
the thin stdio helpers around it.

## Cooking

Cooking an asset is a copy that drops every chunk with `EDITOR_ONLY` set and
sets `COOKED` in the header. The result is the same format read by the same
loader, only smaller. `content_hash` is kept so a cooked file can still be
matched to the editor file it came from.

## Open items

- Payload layouts for `NAME`, `IMPS`, `SRC `, and the texture and mesh chunks.
- The `dump` tool and its `textconv` setup.
- GUID generation and the editor's path-to-GUID index.
- Platform-specific compiled data (BC7 on desktop, ASTC on mobile) is not
  addressed; version 1 targets desktop Vulkan only.
