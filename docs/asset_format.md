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

The container is implemented in `engine/core/include/engine/asset/`:
`asset_view.hpp` (the byte layout and `AssetView` over the prelude),
`asset_writer.hpp` (`AssetWriter` writes, `ASSET_FILE::write_file`),
`asset_reader.hpp` (`AssetReader` reads chunk payloads from the file on
demand, `ASSET_FILE::read_prelude`) and `asset_types/` (the texture and mesh
payload layouts, each with its view and writer). Core has no external
dependencies, so the container stays a pure byte-layout description;
importers live in other modules.

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

**The prelude is the asset; payloads are fetched.** The asset browser, the
dependency scan and every reference to an asset hold only the *prelude*
(header, dependency table, chunk table), which is at the start of the file
and small: an `AssetView` over those bytes answers type, GUID, dependencies
and which chunks exist, with their offsets, sizes and versions. Nothing
loads a whole file. When a payload is needed, an `AssetReader` reopens the
file and seeks straight to that chunk using the view's table, so a mesh's
bounds cost one 48-byte read and a texture's pixels go directly into
staging memory. An asset provider that refreshes an asset re-reads the
prelude; a view from before the file changed is detected (`matches()`
compares GUID, size and content hash) rather than trusted.

**Chunks version themselves.** The container's `format_version` only describes
the layout of the header and tables. Each chunk carries its own `version`, so a
texture importer can change the layout of its pixel chunk without touching
meshes, and only that importer needs a migration. Readers skip chunk tags they
do not know, so adding a chunk is never a breaking change.

**Payloads are plain data.** A chunk's payload is a plain struct or a flat
array: no pointers, no padding surprises, offsets or indices instead. Loading
a chunk is one read into an aligned buffer, a validation pass, and pointers
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

`AssetView` validates every entry against `file_size`, not against the
buffer it was handed, so the prelude validates the same way a complete file
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

Files written before these layouts existed carry the tags with `size == 0`
and `version == 0`; the readers report them as an unsupported version and the
editor re-imports them in place.

## Texture payloads

Defined in `engine/core/include/engine/asset/texture_asset.hpp`
(`TextureAssetView` reads). A texture is one `TEX2` chunk and one `PIXL`
chunk, both at `TEXTURE_ASSET::VERSION` (1). The `TEX2` payload is small and
describes everything; `PIXL` is only bytes. A loader that has read the file
up to the end of `TEX2` can create the image and size its staging buffer
before a single pixel is in memory.

**Formats are engine enums.** `TextureFormat` names the texel layout
(`RGBA8_SRGB`, `BC7_UNORM`, ...) with no reference to Vulkan, so core stays
free of GPU headers and the graphics module owns the one table that maps to
`VkFormat`. Block-compressed formats are first-class: every size in the file
is in whole blocks, so a BC7 file and an RGBA8 file are read by the same
code. sRGB is part of the format, not a flag. Enum values are on disk, so
new formats are appended and never renumbered.

**Compression is a field, reserved.** `TextureDesc::compression` says how
the bytes in `PIXL` are encoded. Version 1 only knows
`TEXTURE_COMPRESSION_NONE`, where every mip is raw texel data ready for
`vkCmdCopyBufferToImage`, and the reader rejects anything else. When a codec
is added it is applied per mip, so the mip table's `size` becomes the stored
size and `decoded_size` in the desc gives the staging buffer. This is kept
separate from the container's `COMPRESSED` chunk flag on purpose: the
container cannot know mip boundaries, and a texture loader wants to stream
and decode one mip at a time.

**Mips are a table, not a formula.** Mip extents do follow from mip 0
(halve and clamp to 1) and the reader verifies that, but the offsets and
pitches are written out so a loader never computes a block size: it copies
`size` bytes from `offset` with `row_pitch` and is done. The table costs 32
bytes per mip.

### TextureDesc (64 bytes)

| offset | type     | field          | meaning |
|-------:|----------|----------------|---------|
| 0      | `u32`    | `width`        | mip 0 width in texels, nonzero |
| 4      | `u32`    | `height`       | mip 0 height in texels, nonzero |
| 8      | `u32`    | `depth`        | mip 0 depth in texels; 1 unless 3D |
| 12     | `u32`    | `layers`       | array layers, nonzero; a multiple of 6 for cube maps |
| 16     | `u32`    | `mip_count`    | 1 to 16, at most the full chain for the extents |
| 20     | `u32`    | `format`       | `TextureFormat` |
| 24     | `u32`    | `dimension`    | `TextureDimension`: `2D` (0), `CUBE` (1), `3D` (2) |
| 28     | `u32`    | `compression`  | `TextureCompression`; only `NONE` (0) in version 1 |
| 32     | `u64`    | `decoded_size` | bytes of raw texel data over every mip and layer |
| 40     | `u32`    | `flags`        | zero; no bits defined in version 1 |
| 44     | `u32[5]` | `reserved`     | zero |

Dimension rules: `2D` and `CUBE` have `depth == 1`; `CUBE` has `layers % 6
== 0` with faces in Vulkan order (+X, -X, +Y, -Y, +Z, -Z); `3D` has `layers
== 1`. A 2D array is `2D` with `layers > 1`.

### TextureMip (32 bytes, `mip_count` entries after the desc)

| offset | type  | field       | meaning |
|-------:|-------|-------------|---------|
| 0      | `u64` | `offset`    | from the start of `PIXL`; multiple of 16 |
| 8      | `u64` | `size`      | bytes in `PIXL` for this mip, all layers; the decoded size when uncompressed |
| 16     | `u32` | `width`     | `max(1, desc.width >> level)` |
| 20     | `u32` | `height`    | `max(1, desc.height >> level)` |
| 24     | `u32` | `depth`     | `max(1, desc.depth >> level)` |
| 28     | `u32` | `row_pitch` | bytes per row of blocks: `ceil(width / block_width) * block_bytes` |

Inside a mip the layers are back to back, each `row_pitch * ceil(height /
block_height) * depth` bytes. Mips are in level order and do not overlap;
the first starts at offset 0 and each starts on a 16-byte boundary, which
with the chunk's own 64-byte alignment satisfies every `bufferOffset`
requirement of a buffer-to-image copy. `PIXL` may be longer than the last
mip's end.

`TextureAssetView::parse` takes the prelude view and the `TEX2` bytes (from
`AssetReader::read_chunk`) and validates all of the above against the
`PIXL` chunk entry, whose size is in the prelude. It keeps that entry as
`pixels_chunk`; the loader reads it into upload memory and uses the view's
mip table to address the result. Pixels are never read to validate a
texture.

### Writing a texture

`TextureAssetWriter` (same header) is the other half: it takes a
`TextureSource` (decoded mip 0 of every layer, tightly packed, plus the
dimensions, format and dimension kind) and a `TextureImportOptions`, and
produces exactly the `TEX2` and `PIXL` payloads described above;
`add_chunks()` appends them to an `AssetWriter`. It writes nothing else: the
GUID, dependencies and editor chunks belong to the importer that decoded the
file. The import options are deliberately thin until the `IMPS` layout
exists: `generate_mips` and `srgb`.

**Mips are generated here, not in the loader.** With `generate_mips` the
writer lays out the full chain (capped at 16 levels) and box filters each
level from the previous one, per layer, following the same halve-and-clamp
rule the reader verifies: a 2x2 (2x2x2 for 3D) window per target texel, the
last row or column of an odd extent dropped. Linear UNORM channels are
averaged as integers so the result is exact and identical everywhere; float
and half channels go through float. Block compressed sources cannot be
filtered, so `generate_mips` is an error for them and they are written as
given, mip 0 only; an importer that compresses supplies its own chain later.

**sRGB is an import decision, filtered in linear space.** `srgb` switches
the stored format to its sRGB variant (`RGBA8`, `BC1`, `BC3`, `BC7`; others
are unchanged) without touching the bytes of mip 0. The mip filter then
decodes the color channels to linear, averages, and re-encodes, so a
checkerboard of black and white halves to the perceptual middle (188, not
128). Alpha stays linear.

## Mesh payloads

Defined in `engine/core/include/engine/asset/mesh_asset.hpp`
(`MeshAssetView` reads). A mesh is one `MESH` chunk, one `VERT` chunk per
vertex stream, one `INDX` chunk and one `BBOX` chunk, all at
`MESH_ASSET::VERSION` (1).

**Streams are chunks.** Each vertex stream is its own `VERT` chunk, 64-byte
aligned, holding `vertex_count` vertices of that stream's stride. The
`i`-th `VERT` chunk in the table is stream `i`; there are exactly
`stream_count` of them. A stream maps one-to-one to a vertex input binding,
so a loader uploads each chunk into a buffer and binds them in order, and a
depth-only pass can read the position stream alone.

**The layout is data, not a fixed vertex struct.** Attributes name a
semantic, an element format, a stream and a byte offset, which is exactly a
`VkVertexInputAttributeDescription`. The importer decides whether to
interleave or split, and whether normals are `F32x3` or `SNORM16x4`; the
file says what it did. `VertexFormat` values are on disk and only appended.

**Submeshes reference materials through the dependency table.** A submesh
is an index range and a material; the material is a `u32` index into the
file's dependency table (so the loader already knows to load it) or
`MESH_ASSET::NO_MATERIAL`. Each submesh carries its own object-space box
for per-submesh culling.

**Always indexed, always triangle lists.** `topology` and `index_format`
are fields so the reader can reject what it does not support rather than
guess; version 1 writes and reads only `TRIANGLE_LIST`, with 16- or 32-bit
indices. Mesh compression (quantization beyond the vertex formats, index
reordering, meshoptimizer-style encoding) is not addressed and would be a
version 2 of these chunks; `MeshDesc::flags` is reserved for it.

### MeshDesc (64 bytes)

| offset | type     | field             | meaning |
|-------:|----------|-------------------|---------|
| 0      | `u32`    | `vertex_count`    | nonzero |
| 4      | `u32`    | `index_count`     | nonzero, multiple of 3 |
| 8      | `u32`    | `index_format`    | `MeshIndexFormat`: `U16` (0) or `U32` (1) |
| 12     | `u32`    | `topology`        | `MeshTopology`; only `TRIANGLE_LIST` (0) in version 1 |
| 16     | `u32`    | `stream_count`    | 1 to 16 `VERT` chunks |
| 20     | `u32`    | `attribute_count` | 1 to 16 |
| 24     | `u32`    | `submesh_count`   | nonzero |
| 28     | `u32`    | `flags`           | zero; no bits defined in version 1 |
| 32     | `u32[8]` | `reserved`        | zero |

The `MESH` payload is this struct followed, back to back, by
`stream_count` `VertexStreamDesc`, `attribute_count`
`VertexAttributeDesc` and `submesh_count` `SubmeshDesc`; its size is
exactly the sum.

### VertexStreamDesc (8 bytes)

| offset | type  | field      | meaning |
|-------:|-------|------------|---------|
| 0      | `u32` | `stride`   | bytes per vertex, nonzero, multiple of 4 |
| 4      | `u32` | `reserved` | zero |

The matching `VERT` chunk is exactly `stride * vertex_count` bytes.

### VertexAttributeDesc (8 bytes)

| offset | type | field            | meaning |
|-------:|------|------------------|---------|
| 0      | `u8` | `semantic`       | `VertexSemantic`: `POSITION` (0), `NORMAL` (1), `TANGENT` (2), `COLOR` (3), `TEXCOORD` (4), `JOINTS` (5), `WEIGHTS` (6) |
| 1      | `u8` | `semantic_index` | distinguishes `TEXCOORD` 0 from `TEXCOORD` 1; the pair is unique within a mesh |
| 2      | `u8` | `format`         | `VertexFormat` |
| 3      | `u8` | `stream`         | index into the stream table |
| 4      | `u32`| `offset`         | bytes from the start of the vertex inside its stream; `offset + size(format) <= stride` |

Vertex formats in version 1: `F32`, `F32x2`, `F32x3`, `F32x4`, `F16x2`,
`F16x4`, `UNORM8x4`, `SNORM8x4`, `UNORM16x2`, `UNORM16x4`, `SNORM16x2`,
`SNORM16x4`, `UINT8x4`, `UINT16x4`, `UINT32`. Tangents are `xyz` plus `w`
handedness.

### SubmeshDesc (48 bytes)

| offset | type     | field         | meaning |
|-------:|----------|---------------|---------|
| 0      | `u32`    | `first_index` | into `INDX` |
| 4      | `u32`    | `index_count` | nonzero, multiple of 3; `first_index + index_count <= desc.index_count` |
| 8      | `u32`    | `base_vertex` | added to every index; `< vertex_count` |
| 12     | `u32`    | `material`    | dependency table index, or `0xffffffff` for none |
| 16     | `f32[3]` | `bounds_min`  | object-space box over this submesh |
| 28     | `f32[3]` | `bounds_max`  | |
| 40     | `u32[2]` | `reserved`    | zero |

### INDX

`index_count` indices of `index_format` size, tightly packed; the chunk is
exactly that many bytes.

### MeshBounds (48 bytes, the `BBOX` payload)

| offset | type     | field      | meaning |
|-------:|----------|------------|---------|
| 0      | `f32[3]` | `min`      | object-space box over the whole mesh |
| 12     | `f32[3]` | `max`      | |
| 24     | `f32[3]` | `center`   | bounding sphere |
| 36     | `f32`    | `radius`   | |
| 40     | `u32[2]` | `reserved` | zero |

`MeshAssetView::parse` takes the prelude view and the `MESH` bytes (from
`AssetReader::read_chunk`) and validates the tables against the `VERT`,
`INDX` and `BBOX` chunk entries, whose sizes are in the prelude. It keeps
those entries (`vertex_chunks[i]`, `index_chunk`, `bounds_chunk`); the
loader reads whichever it needs, straight into upload memory, and a culling
pass or the editor can read the bounds alone. No geometry is read to
validate a mesh.

### Writing a mesh

`MeshAssetWriter` (same header) takes a `MeshSource` (the vertex streams
with their strides and bytes, the attribute table, 32-bit indices and an
optional submesh list) and a `MeshImportOptions`, and produces the `MESH`,
`VERT`, `INDX` and `BBOX` payloads; `add_chunks()` appends them to an
`AssetWriter`, streams in order. Like the texture writer it owns no
container concerns; a submesh's `material` is an index into the dependency
table the importer is building, which the writer cannot check and the
reader does.

**The importer owns the vertex layout; the writer checks and copies it.**
Interleaving, quantization and attribute formats are decided by whoever
decoded the source. The writer enforces exactly what the reader will
(strides, attribute fit, unique semantics) and copies the bytes unchanged,
so what goes to the GPU is what the importer laid out.

**Indices are narrowed, never widened.** The source always hands over
`u32` indices; with `compact_indices` (the default) the writer stores
`U16` when `vertex_count <= 65536`, since every index is checked to be
below `vertex_count`, and `U32` otherwise. `base_vertex` is written as 0.

**Bounds are derived, not supplied.** A `POSITION 0` attribute in `F32x3`
or `F32x4` is required. The `BBOX` box spans every vertex, its sphere is
centered on the box and sized to the farthest vertex; each submesh's box
spans only the vertices its indices reach. A source with no submeshes gets
one covering every index with no material.

## Reading

Reading is two steps with two types, so the cheap one can be done for every
file in a project and the expensive one only for what is used.

**The prelude: `ASSET_FILE::read_prelude` + `AssetView`.** `read_prelude(path,
allocator)` opens the file, reads the header, then exactly the bytes the
tables need, and returns the parsed `AssetView` over them (`free_prelude`
releases the buffer, which is the view's `data`). `AssetView::parse(data,
size)` is the validation on its own, for a buffer from anywhere; it returns
a view whose `is_ok()` says whether it passed and whose `parse_error` says
why not:

1. `size >= sizeof(AssetHeader)`, magic matches, `format_version` is supported.
2. the guid is not null and `file_size` is at least the prelude size
   (`parse_header`, the checks that need only the header).
3. `size <= file_size` (a buffer longer than the declared file is not this
   file) and `size` covers the prelude.
4. every chunk entry is 64-byte aligned, starts at or after `payload_start`,
   ends at or before `file_size`, and does not overlap the previous entry.

A view is what the asset browser lists and what a reference to an asset
holds. It never touches a payload; it has no way to.

**Payloads: `AssetReader`.** `open(path)` reopens the file, reads the header
back (`parse_header`) and checks that the file on disk is exactly
`file_size` bytes, so a truncated or half-written file is refused before
any seek. `matches(view)` checks the open file against a view (GUID,
`file_size`, `content_hash`), which catches a view from a stale scan. Then:

- `read_chunk(entry, out)` seeks to `entry.offset` and reads `entry.size`
  bytes into caller memory (a struct on the stack for `BBOX`, a mapped
  staging buffer for `PIXL`);
- `read_chunk(entry, allocator)` does the same into a fresh 64-byte aligned
  buffer;
- `read_chunk(view, tag, allocator)` finds the first chunk with `tag` in the
  view and reads it into a fresh buffer, returning a `ReadChunk`: the bytes
  plus the entry they belong to, or a `read_error` saying the view is stale
  (`ASSET_READ_STALE_VIEW`), the tag is absent (`ASSET_READ_NOT_FOUND`) or
  the read failed. An empty chunk is ok with no bytes;
- `read_prelude(allocator)` re-reads the tables into a new view, which is
  how a view is refreshed after the file changed.

Every entry is bounds-checked against the header read at `open`, never
against the view, so a corrupt entry cannot read outside the file. Chunks
are read in any order; the reader seeks for each one.

The per-type views (`TextureAssetView`, `MeshAssetView`, in `asset_types/`)
take the prelude view plus the bytes of their descriptor chunk (or the
`ReadChunk` straight from the reader), validate them against the other
chunks' table entries, and hand back the entries of the big chunks for the
caller to read where it wants them.

The runtime loader additionally rejects a file without `COOKED` set, so a
shipped build cannot carry sources by mistake. The editor accepts both.

## The asset provider

`AssetResourceProvider` (`asset_resource_provider.hpp`) is where payloads
live once read. The engine creates one as a singleton; everything that
needs an asset's bytes asks it, so a texture referenced by ten materials
is read once and resident in one place.

**Registration is a view and a path.** `add(view, path)` makes an asset
known under the GUID in its header. The provider copies the prelude, so
the `AssetView` it keeps outlives the scan buffer, and keeps the path, which
is the one thing a view does not carry. Nothing is read at this point: a
project scan registers every file it finds for the cost of the preludes it
already read. `require_cooked` makes `add` refuse uncooked files, which the
standalone runtime turns on.

**Loading is lazy and per chunk.** `get(guid)` (or `get(view)`, which uses
the view's GUID) returns the asset's `AssetResource` with every runtime
chunk resident: whatever was already in memory, or read now. A read opens
the file with an `AssetReader` and fills one 64-byte aligned buffer per
chunk, allocated and owned by the provider, so the payload is used in
place as the format intends. `EDITOR_ONLY` chunks are not part of a load;
`get_chunk(resource, tag)` reads a single chunk on demand, which is how the
editor reaches the source bytes without paying for them otherwise. A
second `get` finds the payloads resident and touches no file.

**Stale preludes are refreshed, never trusted.** Before any read the
provider checks the file against its prelude (`AssetReader::matches`). If
the file was re-imported since, the prelude is re-read from the file and
every payload read from the old version is dropped, so a resource never
mixes bytes from two chunk tables. A file that now holds a different GUID
is an error. `add` with a newer view of a known GUID does the same without
touching disk.

**Streaming out is `unload`.** It frees the payloads and keeps the resource
known, so the next `get` reads them again; `remove` forgets the asset. The
provider reports `resident_bytes()` over every resource for whoever decides
what to evict; it does not decide itself. Reads are synchronous on the
calling thread; asynchronous streaming would sit on top of the same
resources.

## Writing

`AssetWriter` collects dependencies and chunks, then `write()` emits the file
in one pass: it computes the prelude size from the counts, places payloads in
the order they were added, 64-byte aligned, and fills `file_size`. The writer
never pads the end of the file. `ASSET_FILE::write_file` is the thin
platform-file helper that puts the bytes on disk.

## Cooking

Cooking an asset is a copy that drops every chunk with `EDITOR_ONLY` set and
sets `COOKED` in the header: read the prelude, open a reader, and for each
chunk that is not editor-only `read_chunk` it and `add_chunk` it to a
writer. The result is the same format read by the same loader, only smaller. `content_hash` is kept so a cooked file can still be
matched to the editor file it came from.

## Open items

- Payload layouts for `NAME`, `IMPS` (the `TextureImportOptions` and
  `MeshImportOptions` structs are the in-memory shape; what gets serialized
  is still to be decided) and `SRC `.
- Importers that decode source files (PNG, glTF) into `TextureSource` and
  `MeshSource`; core has no decoders, so they live in the editor.
- The `dump` tool and its `textconv` setup.
- Asynchronous loading and an eviction policy for the asset provider.
- GUID generation and the editor's path-to-GUID index.
- Platform-specific compiled data (BC7 on desktop, ASTC on mobile) is not
  addressed; version 1 targets desktop Vulkan only.
