#include "import/obj_importer.hpp"

#include "import/importer.hpp"

#include "engine/asset/asset_writer.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <charconv>
#include <cmath>
#include <unordered_map>

// --- ObjMesh ------------------------------------------------------------------------

ObjMesh::ObjMesh() : ObjMesh(MEMORY::heap_allocator()) {}

ObjMesh::ObjMesh(BaseAllocator* allocator)
    : vertices(allocator), indices(allocator), submeshes(allocator), submesh_materials(allocator) {}

MeshSource ObjMesh::source() const {
    MeshSource source;
    source.vertex_count = static_cast<u32>(this->vertices.count);
    source.stream_count = 1;
    source.streams[0].stride = sizeof(ObjVertex);
    source.streams[0].vertices = this->vertices.data;
    source.attribute_count = 3;
    source.attributes[0] = {VERTEX_SEMANTIC_POSITION, 0, VERTEX_FORMAT_F32x3, 0, offsetof(ObjVertex, position)};
    source.attributes[1] = {VERTEX_SEMANTIC_NORMAL, 0, VERTEX_FORMAT_F32x3, 0, offsetof(ObjVertex, normal)};
    source.attributes[2] = {VERTEX_SEMANTIC_TEXCOORD, 0, VERTEX_FORMAT_F32x2, 0, offsetof(ObjVertex, texcoord)};
    source.indices = this->indices.data;
    source.index_count = static_cast<u32>(this->indices.count);
    source.submeshes = this->submeshes.data;
    source.submesh_count = static_cast<u32>(this->submeshes.count);
    return source;
}

void ObjMesh::clear() {
    this->vertices.clear();
    this->indices.clear();
    this->submeshes.clear();
    this->submesh_materials.clear();
    this->materials.clear();
    this->has_normals = false;
    this->has_texcoords = false;
}

void ObjMesh::free() {
    this->vertices.free();
    this->indices.free();
    this->submeshes.free();
    this->submesh_materials.free();
    this->materials.clear();
    this->materials.shrink_to_fit();
    this->has_normals = false;
    this->has_texcoords = false;
}

// --- Parsing --------------------------------------------------------------------------

const char* OBJ::parse_error_name(const ObjParseError error) {
    switch (error) {
    case OBJ_PARSE_OK: return "ok";
    case OBJ_PARSE_BAD_NUMBER: return "a value is not a number";
    case OBJ_PARSE_BAD_FACE: return "a face has fewer than three corners or a malformed corner";
    case OBJ_PARSE_BAD_INDEX: return "a face refers to a vertex that does not exist";
    case OBJ_PARSE_NO_FACES: return "the file has no faces";
    case OBJ_PARSE_TOO_MANY: return "too many vertices or indices for 32-bit indices";
    }
    return "unknown error";
}

namespace {

constexpr u32 NONE = ~0u;

struct Vec3 {
    f32 x = 0, y = 0, z = 0;
};
struct Vec2 {
    f32 x = 0, y = 0;
};

// One `f` corner as written: resolved 0-based indices, NONE when absent.
struct Corner {
    u32 position = NONE;
    u32 texcoord = NONE;
    u32 normal = NONE;

    bool operator==(const Corner& other) const {
        return this->position == other.position && this->texcoord == other.texcoord && this->normal == other.normal;
    }
};

struct CornerHash {
    usz operator()(const Corner& corner) const {
        u64 hash = corner.position;
        hash = hash * 0x9e3779b97f4a7c15ull ^ corner.texcoord;
        hash = hash * 0x9e3779b97f4a7c15ull ^ corner.normal;
        return static_cast<usz>(hash ^ (hash >> 29));
    }
};

bool is_space(const char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// A line of the file with a cursor: tokens are read left to right.
struct Line {
    const char* at;
    const char* end;

    void skip_spaces() {
        while (this->at < this->end && is_space(*this->at)) {
            ++this->at;
        }
    }
    bool at_end() {
        this->skip_spaces();
        return this->at == this->end;
    }
    // The next run of non-space characters, empty at the end of the line.
    std::string_view token() {
        this->skip_spaces();
        const char* start = this->at;
        while (this->at < this->end && !is_space(*this->at)) {
            ++this->at;
        }
        return std::string_view(start, static_cast<usz>(this->at - start));
    }
    bool number(f32* out) {
        const std::string_view text = this->token();
        const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), *out);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }
};

// Parses one `a`, `a/b`, `a//c` or `a/b/c` corner. `counts` are how many
// positions, texcoords and normals exist so far, for negative and range checks.
ObjParseError parse_corner(const std::string_view text, const u32 counts[3], Corner* out) {
    u32* fields[3] = {&out->position, &out->texcoord, &out->normal};
    usz field = 0;
    usz start = 0;
    while (field < 3) {
        usz slash = text.find('/', start);
        const usz stop = slash == std::string_view::npos ? text.size() : slash;
        if (stop > start) {
            i64 index = 0;
            const std::from_chars_result result = std::from_chars(text.data() + start, text.data() + stop, index);
            if (result.ec != std::errc{} || result.ptr != text.data() + stop) {
                return OBJ_PARSE_BAD_FACE;
            }
            // OBJ indices are 1-based; negative ones count back from the end.
            if (index < 0) {
                index += counts[field];
            } else {
                index -= 1;
            }
            if (index < 0 || index >= static_cast<i64>(counts[field])) {
                return OBJ_PARSE_BAD_INDEX;
            }
            *fields[field] = static_cast<u32>(index);
        } else if (field == 0) {
            return OBJ_PARSE_BAD_FACE; // the position is never optional
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
        ++field;
    }
    return OBJ_PARSE_OK;
}

void normalize(f32* v) {
    const f32 length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
}

struct Parser {
    ObjMesh& mesh;
    std::vector<Vec3> positions;
    std::vector<Vec2> texcoords;
    std::vector<Vec3> normals;
    std::unordered_map<Corner, u32, CornerHash> vertex_of_corner;
    std::vector<Corner> face; // corners of the face being read
    u32 current_material = NONE;
    u32 submesh_first_index = 0;

    explicit Parser(ObjMesh& mesh) : mesh(mesh) {}

    // Closes the submesh the faces so far belong to, if it has any.
    void close_submesh() {
        const u32 index_count = static_cast<u32>(this->mesh.indices.count) - this->submesh_first_index;
        if (index_count == 0) {
            return;
        }
        MeshSourceSubmesh submesh;
        submesh.first_index = this->submesh_first_index;
        submesh.index_count = index_count;
        submesh.material = MESH_ASSET::NO_MATERIAL;
        this->mesh.submeshes.push(submesh);
        this->mesh.submesh_materials.push(this->current_material);
        this->submesh_first_index = static_cast<u32>(this->mesh.indices.count);
    }

    void use_material(const std::string_view name) {
        this->close_submesh();
        for (usz i = 0; i < this->mesh.materials.size(); ++i) {
            if (this->mesh.materials[i] == name) {
                this->current_material = static_cast<u32>(i);
                return;
            }
        }
        this->mesh.materials.emplace_back(name);
        this->current_material = static_cast<u32>(this->mesh.materials.size() - 1);
    }

    // The vertex for a corner, made on first use.
    u32 vertex(const Corner& corner) {
        const auto found = this->vertex_of_corner.find(corner);
        if (found != this->vertex_of_corner.end()) {
            return found->second;
        }
        ObjVertex vertex = {};
        const Vec3& position = this->positions[corner.position];
        vertex.position[0] = position.x;
        vertex.position[1] = position.y;
        vertex.position[2] = position.z;
        if (corner.texcoord != NONE) {
            const Vec2& texcoord = this->texcoords[corner.texcoord];
            vertex.texcoord[0] = texcoord.x;
            vertex.texcoord[1] = 1.0f - texcoord.y; // bottom-left origin to top-left
        }
        if (corner.normal != NONE) {
            const Vec3& normal = this->normals[corner.normal];
            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
        }
        const u32 index = static_cast<u32>(this->mesh.vertices.count);
        this->mesh.vertices.push(vertex);
        this->vertex_of_corner.emplace(corner, index);
        return index;
    }

    ObjParseError read_face(Line& line) {
        this->face.clear();
        const u32 counts[3] = {static_cast<u32>(this->positions.size()), static_cast<u32>(this->texcoords.size()),
                               static_cast<u32>(this->normals.size())};
        while (!line.at_end()) {
            Corner corner;
            const ObjParseError error = parse_corner(line.token(), counts, &corner);
            if (error != OBJ_PARSE_OK) {
                return error;
            }
            this->face.push_back(corner);
        }
        if (this->face.size() < 3) {
            return OBJ_PARSE_BAD_FACE;
        }
        // Fan triangulation keeps the file's winding (counter-clockwise front).
        const u32 first = this->vertex(this->face[0]);
        u32 previous = this->vertex(this->face[1]);
        for (usz i = 2; i < this->face.size(); ++i) {
            const u32 next = this->vertex(this->face[i]);
            this->mesh.indices.push(first);
            this->mesh.indices.push(previous);
            this->mesh.indices.push(next);
            previous = next;
        }
        return OBJ_PARSE_OK;
    }

    // Smooth normals for a file without `vn`: every triangle's face normal,
    // weighted by its area, summed into its three vertices.
    void generate_normals() {
        for (usz i = 0; i + 2 < this->mesh.indices.count; i += 3) {
            ObjVertex& a = this->mesh.vertices[this->mesh.indices[i]];
            ObjVertex& b = this->mesh.vertices[this->mesh.indices[i + 1]];
            ObjVertex& c = this->mesh.vertices[this->mesh.indices[i + 2]];
            const f32 ab[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
            const f32 ac[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
            const f32 normal[3] = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]};
            for (ObjVertex* vertex : {&a, &b, &c}) {
                vertex->normal[0] += normal[0];
                vertex->normal[1] += normal[1];
                vertex->normal[2] += normal[2];
            }
        }
        for (ObjVertex& vertex : this->mesh.vertices) {
            normalize(vertex.normal);
        }
    }
};

} // namespace

ObjParseError OBJ::parse(const char* text, const usz size, ObjMesh* out, u32* out_line) {
    out->clear();
    if (out_line != nullptr) {
        *out_line = 0;
    }
    Parser parser(*out);

    const char* at = text;
    const char* const end = text + size;
    u32 line_number = 0;
    ObjParseError error = OBJ_PARSE_OK;
    while (at < end && error == OBJ_PARSE_OK) {
        ++line_number;
        const char* line_end = at;
        while (line_end < end && *line_end != '\n') {
            ++line_end;
        }
        Line line{at, line_end};
        at = line_end + 1;

        // Everything after '#' is a comment.
        for (const char* c = line.at; c < line.end; ++c) {
            if (*c == '#') {
                line.end = c;
                break;
            }
        }

        const std::string_view keyword = line.token();
        if (keyword == "v") {
            Vec3 position;
            if (!line.number(&position.x) || !line.number(&position.y) || !line.number(&position.z)) {
                error = OBJ_PARSE_BAD_NUMBER;
            }
            parser.positions.push_back(position);
        } else if (keyword == "vt") {
            Vec2 texcoord;
            if (!line.number(&texcoord.x) || !line.number(&texcoord.y)) {
                error = OBJ_PARSE_BAD_NUMBER;
            }
            parser.texcoords.push_back(texcoord);
        } else if (keyword == "vn") {
            Vec3 normal;
            if (!line.number(&normal.x) || !line.number(&normal.y) || !line.number(&normal.z)) {
                error = OBJ_PARSE_BAD_NUMBER;
            }
            parser.normals.push_back(normal);
        } else if (keyword == "f") {
            error = parser.read_face(line);
        } else if (keyword == "usemtl") {
            parser.use_material(line.token());
        }
        // o, g, s, mtllib, empty lines and anything unknown are skipped.

        if (out->vertices.count > 0xffffffffull || out->indices.count > 0xffffffffull) {
            error = OBJ_PARSE_TOO_MANY;
        }
    }

    if (error == OBJ_PARSE_OK && out->indices.count == 0) {
        error = OBJ_PARSE_NO_FACES;
        line_number = 0;
    }
    if (error != OBJ_PARSE_OK) {
        out->clear();
        if (out_line != nullptr) {
            *out_line = line_number;
        }
        return error;
    }

    parser.close_submesh();
    out->has_texcoords = !parser.texcoords.empty();
    out->has_normals = !parser.normals.empty();
    if (!out->has_normals) {
        parser.generate_normals();
    }
    return OBJ_PARSE_OK;
}

// --- Import -----------------------------------------------------------------------------

bool OBJ::import(
	const std::filesystem::path& source, 
	const std::filesystem::path& destination, 
	const MeshImportOptions& options,
    const AssetGuid& guid, 
    std::string* error
) {
    const std::string source_path = source.string();
    usz size = 0;
    u8* text = IMPORT::read_file(source_path.c_str(), MEMORY::heap_allocator(), &size, error);
    if (text == nullptr) {
        return false;
    }

    ObjMesh mesh;
    u32 line = 0;
    const ObjParseError parse_error = OBJ::parse(reinterpret_cast<const char*>(text), size, &mesh, &line);
    if (parse_error != OBJ_PARSE_OK) {
        *error = source_path + ":" + std::to_string(line) + ": " + OBJ::parse_error_name(parse_error);
        mesh.free();
        MEMORY::heap_allocator()->free(text);
        return false;
    }

    MeshAssetWriter payloads;
    const MeshWriteError write_error = payloads.build(mesh.source(), options);
    if (write_error != MESH_WRITE_OK) {
        *error = source_path + ": " + MESH_ASSET::write_error_name(write_error);
        payloads.free();
        mesh.free();
        MEMORY::heap_allocator()->free(text);
        return false;
    }

    AssetWriter writer;
    writer.type = ASSET_TYPE::MESH;
    writer.guid = guid;
    writer.content_hash = IMPORT::fnv1a(text, size);
    IMPORT::add_editor_chunks(writer);
    payloads.add_chunks(writer);
    const bool ok = IMPORT::write_asset(writer, destination.string().c_str(), error);

    writer.free();
    payloads.free();
    mesh.free();
    MEMORY::heap_allocator()->free(text);
    return ok;
}
