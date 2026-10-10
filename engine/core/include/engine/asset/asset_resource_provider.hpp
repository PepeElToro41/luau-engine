#pragma once

#include "engine/asset/asset_reader.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/hash_map.hpp"

// Streams asset payloads in and out of memory, keyed by GUID. The engine
// creates one as a singleton (engine.get_singleton<AssetResourceProvider>())
// and everything that needs an asset's bytes goes through it, so a texture
// referenced by ten materials is read from disk once and lives in one place.
//
//     AssetResourceProvider* provider = engine.get_singleton<AssetResourceProvider>();
//     provider->add(view, "assets/rock.lunaasset");        // the scan registers every file it finds
//     ...
//     AssetResource* rock = provider->get(view);             // loads on first use, returns the same object after
//     const ChunkEntry* entry = nullptr;
//     const u8* mesh = rock->find_payload(CHUNK_TYPE::MESH, &entry);
//     MeshAssetView geometry;
//     geometry.parse(rock->view, mesh, entry->size);
//     ...
//     provider->unload(rock->guid);                          // streams the payloads out; the asset stays known
//
// An asset enters the provider through add(view, path): the AssetView is
// the prelude an asset scan produced (ASSET_FILE::read_prelude, or
// TEXT_ASSET::read_prelude for a text asset such as a .material; see
// text_asset.hpp), its header's GUID is the key, and the provider copies
// the prelude so the view it keeps outlives whatever buffer the caller
// parsed. Nothing is read from disk
// until get() or get_chunk() asks for it: get() opens the file with an
// AssetReader and reads every runtime chunk (the ones not EDITOR_ONLY) into
// memory the provider owns, one 64-byte aligned buffer per chunk, and
// get_chunk() reads a single chunk, which is how the editor reaches an
// EDITOR_ONLY chunk such as the source bytes without loading them all. A
// second get() finds the payloads resident and touches no file.
//
// Before reading, the provider checks the file against the prelude it holds
// (AssetReader::matches). If the file was re-imported since the prelude was
// taken, the prelude is re-read from the file and any payloads read from the
// old file are dropped, so a resource never mixes bytes from two versions.
// refresh() does the same on demand.
//
// A text asset (ASSET_FLAG::TEXT in its header) is read as a whole: its one
// TEXT chunk is the file's bytes, and the staleness check hashes them
// (TEXT_ASSET::matches); a file whose text changed gets a fresh prelude the
// same way. Everything else is the same: get(), unload(), refresh(),
// find_payload(CHUNK_TYPE::TEXT).
//
// Pointers into a resource (its view, its payloads) stay valid until the
// resource is unloaded, refreshed or removed, or the provider is freed;
// they survive other assets being added. Everything is single-threaded:
// reads happen on the calling thread, when asked for.
//
// There is no destructor: free() releases every resource. The engine calls
// it from shutdown(), before the singletons are destroyed.

// One asset the provider knows about: where it is, its prelude and whatever
// payloads are resident. Owned by the provider; read-only for everyone else.
struct AssetResource {
    AssetGuid guid;
    // UTF-8 path add() was given, owned by the provider.
    const char* path = nullptr;

    // The view over the provider's copy of the prelude; always ok(). Its
    // `data` / `size` are the copied bytes.
    AssetView view;

    // One entry per chunk in `view`: the payload bytes, or nullptr when the
    // chunk is not resident or is empty. Each buffer is PAYLOAD_ALIGNMENT
    // aligned and exactly the chunk's size.
    u8** payloads = nullptr;
    // Bytes of payloads currently resident.
    u64 resident_bytes = 0;

    // A text asset (text_asset.hpp): the path is a text file whose bytes are
    // the one TEXT chunk.
    bool is_text() const { return this->view.is_text(); }

    // Whether every runtime chunk (not EDITOR_ONLY) is resident: what get()
    // guarantees. EDITOR_ONLY chunks are not counted either way.
    bool is_loaded() const;
    // Whether chunk `chunk` is in memory. An empty chunk always is: there is
    // nothing to read.
    bool is_resident(usz chunk) const;

    // The payload of chunk `chunk`, or nullptr when it is not resident, is
    // empty or `chunk` is out of range. Never reads; see
    // AssetResourceProvider::get_chunk for that.
    const u8* payload(usz chunk) const;
    // The payload of the first chunk tagged `tag`. `out_chunk`, when given,
    // receives its entry (nullptr when the tag is absent) so the caller has
    // the size and version; the entry is set even when the chunk is not
    // resident or empty and the result is nullptr.
    const u8* find_payload(u32 tag, const ChunkEntry** out_chunk = nullptr) const;
};

struct AssetResourceProvider {
    AssetResourceProvider();
    explicit AssetResourceProvider(BaseAllocator* allocator);

    AssetResourceProvider(const AssetResourceProvider&) = delete;
    AssetResourceProvider& operator=(const AssetResourceProvider&) = delete;

    // Where resources, preludes and payloads are allocated.
    BaseAllocator* allocator = nullptr;
    // When set, add() refuses a view without ASSET_FLAG::COOKED, so a
    // shipped build cannot pick up editor files. Off by default; the
    // standalone runtime turns it on.
    bool require_cooked = false;

    // --- Registration ---------------------------------------------------------

    // Makes the asset `view` describes, stored at `path`, known under its
    // GUID and returns its resource. The prelude is copied. If the GUID is
    // already known the existing resource is returned with its path updated;
    // if `view` additionally describes a different version of the file
    // (size or content hash differ) the stored prelude is replaced and any
    // resident payloads are dropped. nullptr (and a message on stderr) when
    // `view` is not ok, or is not cooked while require_cooked is set.
    AssetResource* add(const AssetView& view, const char* path);
    // The resource for `guid`, loaded or not, or nullptr if never added.
    AssetResource* find(const AssetGuid& guid) const;
    bool contains(const AssetGuid& guid) const { return this->resources.contains(guid); }
    // Forgets `guid`: releases its payloads and prelude. False if unknown.
    bool remove(const AssetGuid& guid);

    // --- Loading --------------------------------------------------------------

    // The resource for `guid` with every runtime chunk resident: what was
    // already in memory, or read now from the file. nullptr (and a message)
    // when the GUID is unknown or the file cannot be read; what was read
    // before a failure stays resident. A view whose GUID is unknown is not
    // added: the provider has no path for it.
    AssetResource* get(const AssetGuid& guid);
    AssetResource* get(const AssetView& view);

    // A single chunk of `resource`, read now if it is not resident:
    // get_chunk finds the first chunk tagged `tag`, with `out_chunk` as in
    // find_payload, and get_chunk_at addresses `resource->view.chunks[chunk]`.
    // nullptr when the chunk is absent, empty, out of range or cannot be
    // read. If the file changed on disk the prelude is refreshed first,
    // which makes an index taken from the old view meaningless: get_chunk_at
    // then fails and get_chunk looks the tag up again in the new view.
    const u8* get_chunk(AssetResource* resource, u32 tag, const ChunkEntry** out_chunk = nullptr);
    const u8* get_chunk_at(AssetResource* resource, usz chunk);

    // Reads every runtime chunk that is not resident. True when the resource
    // is loaded afterwards. This is what get() does on a miss.
    bool load(AssetResource* resource);

    // Re-reads the prelude when the file on disk no longer matches the one
    // held (re-imported since add()), dropping resident payloads; a file
    // that still matches is left alone. False when the file cannot be opened
    // or now holds a different asset; the resource is then unchanged.
    bool refresh(AssetResource* resource);

    // Streams the payloads out: frees every resident buffer and keeps the
    // resource known, so a later get() reads it again.
    void unload(AssetResource* resource);
    bool unload(const AssetGuid& guid);
    void unload_all();

    // --- Bookkeeping ----------------------------------------------------------

    usz count() const { return this->resources.count; }
    // Bytes of payloads resident over every resource.
    u64 resident_bytes() const { return this->resident_total; }

    // Releases every resource and the storage.
    void free();

private:
    // A resource's file while chunks are read from it: an AssetReader on a
    // .lunaasset, or the whole text of a text asset.
    struct OpenFile {
        AssetReader reader;
        u8* text = nullptr;
        usz text_size = 0;
    };

    HashMap<AssetGuid, AssetResource*, AssetGuidHash> resources;
    u64 resident_total = 0;

    // Gives `resource` the prelude `view` points into, which must be a
    // PAYLOAD_ALIGNMENT aligned buffer from `allocator` that the resource
    // now owns, and an empty payload table sized for it. `resource` must
    // hold no prelude or payloads.
    void adopt_prelude(AssetResource* resource, const AssetView& view);
    // Copies `view`'s bytes and adopts the copy. False when the copy does
    // not parse, which cannot happen for an ok view.
    bool store_prelude(AssetResource* resource, const AssetView& view);
    void free_prelude(AssetResource* resource);
    // Opens `resource`'s file and refreshes the prelude if the file changed.
    // `out_refreshed` tells whether it did. False (file closed) on failure.
    bool open(AssetResource* resource, OpenFile* file, bool* out_refreshed);
    void close(OpenFile* file);
    // Reads chunk `chunk` of `resource` from `file`, which is open on its
    // file and matches its view. True if the chunk is resident afterwards.
    bool read_chunk(AssetResource* resource, OpenFile& file, usz chunk);
    void release(AssetResource* resource);
};
