#pragma once

#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "import/importer.hpp"

#include <filesystem>
#include <string>
#include <vector>

// Wavefront .obj import. OBJ::parse turns the text into an ObjMesh: one
// interleaved vertex stream (position, normal, texcoord), 32-bit indices
// forming a triangle list, and one submesh per `usemtl` run. ObjMesh::source
// describes that as the MeshSource the core's MeshAssetWriter builds from,
// and OBJ::import does the whole file-to-.lunaasset trip.
//
//     std::string error;
//     if (!OBJ::import(ImportSource::file("rock.obj"), "rock.lunaasset", MeshImportOptions{}, IMPORT::random_guid(), &error)) {
//         output.error("%s", error.c_str());
//     }
//
// What is read: `v` (x y z; w ignored), `vt` (u v; w ignored), `vn`, `f` with
// any of the corner forms `v`, `v/vt`, `v//vn`, `v/vt/vn`, positive or
// negative (relative) indices, polygons of any size (triangulated as fans),
// and `usemtl`. `o`, `g`, `s`, `mtllib`, comments and unknown keywords are
// skipped. Every (v, vt, vn) corner triple becomes one vertex, shared by the
// faces that use it. A file with no `vn` gets smooth per-vertex normals
// averaged from its faces; a file with no `vt` gets zero texcoords. The
// texcoord v axis is flipped (1 - v): OBJ's origin is bottom-left, Vulkan's
// top-left. Material names are kept by name only; the submeshes are written
// with MESH_ASSET::NO_MATERIAL until materials exist as assets.

struct ObjVertex {
    f32 position[3];
    f32 normal[3];
    f32 texcoord[2];
};
static_assert(sizeof(ObjVertex) == 32, "ObjVertex is the on-disk vertex layout: 32 bytes");

struct ObjMesh {
    ObjMesh();
    explicit ObjMesh(BaseAllocator* allocator);

    ObjMesh(const ObjMesh&) = delete;
    ObjMesh& operator=(const ObjMesh&) = delete;

    DynamicArray<ObjVertex> vertices;
    DynamicArray<u32> indices;              // triangle list into `vertices`
    DynamicArray<MeshSourceSubmesh> submeshes; // one per `usemtl` run; one for the whole file without any
    // Index into `materials` for each submesh, ~0u for faces before any `usemtl`.
    DynamicArray<u32> submesh_materials;
    std::vector<std::string> materials; // `usemtl` names in first-use order

    bool has_normals = false;   // the file had `vn`; false means they were generated
    bool has_texcoords = false; // the file had `vt`

    // The mesh as the MeshAssetWriter takes it: one stream of ObjVertex with
    // POSITION 0 / NORMAL 0 / TEXCOORD 0 attributes. Points into this object.
    MeshSource source() const;

    bool is_empty() const { return this->vertices.count == 0; }

    void clear();
    void free();
};

enum ObjParseError {
    OBJ_PARSE_OK = 0,
    OBJ_PARSE_BAD_NUMBER,   // a `v`, `vt`, `vn` or `f` value is not a number
    OBJ_PARSE_BAD_FACE,     // a face has fewer than three corners or a malformed corner
    OBJ_PARSE_BAD_INDEX,    // a face refers to a position, texcoord or normal that does not exist
    OBJ_PARSE_NO_FACES,     // the file has no `f` lines
    OBJ_PARSE_TOO_MANY,     // more than 2^32 - 1 vertices or indices
};

namespace OBJ {

const char* parse_error_name(ObjParseError error);

// Parses `size` bytes of .obj text into `out`, which is cleared first. On an
// error `out` is left cleared and `out_line`, when given, is the 1-based line
// the error was found on (0 for OBJ_PARSE_NO_FACES).
ObjParseError parse(const char* text, usz size, ObjMesh* out, u32* out_line = nullptr);

// Reads the bytes of `source` (a file, or a kept original inside an asset:
// importer.hpp), parses them, builds the mesh payloads with `options` and
// writes a mesh asset with `guid` to `destination`, keeping the bytes in
// its SRC chunk when `options.keep_source` is set. The bytes are read
// whole before anything is written, so `destination` may be the asset
// `source` points into. False with `error` set (and no file written) on
// any failure.
bool import(const ImportSource& source, const std::filesystem::path& destination, const MeshImportOptions& options,
            const AssetGuid& guid, std::string* error);

} // namespace OBJ
