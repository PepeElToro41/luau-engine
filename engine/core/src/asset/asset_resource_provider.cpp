#include "engine/asset/asset_resource_provider.hpp"

#include "engine/memory/heap_allocator.hpp"

#include <cstring>
#include <new>

// --- AssetResource ----------------------------------------------------------

bool AssetResource::is_loaded() const {
    const usz chunk_count = this->view.chunk_count();
    for (usz i = 0; i < chunk_count; ++i) {
        if (!this->view.chunks[i].is_editor_only() && !this->is_resident(i)) {
            return false;
        }
    }
    return true;
}

bool AssetResource::is_resident(const usz chunk) const {
    if (chunk >= this->view.chunk_count()) {
        return false;
    }
    return this->view.chunks[chunk].size == 0 || this->payloads[chunk] != nullptr;
}

const u8* AssetResource::payload(const usz chunk) const {
    return chunk < this->view.chunk_count() ? this->payloads[chunk] : nullptr;
}

const u8* AssetResource::find_payload(const u32 tag, const ChunkEntry** out_chunk) const {
    const ChunkEntry* chunk = this->view.find_chunk(tag);
    if (out_chunk != nullptr) {
        *out_chunk = chunk;
    }
    if (chunk == nullptr) {
        return nullptr;
    }
    return this->payloads[static_cast<usz>(chunk - this->view.chunks)];
}

// --- AssetResourceProvider: registration ------------------------------------

AssetResourceProvider::AssetResourceProvider() : AssetResourceProvider(MEMORY::heap_allocator()) {}

AssetResourceProvider::AssetResourceProvider(BaseAllocator* allocator) : allocator(allocator), resources(allocator) {}

static void print_guid(char* out, const usz out_size, const AssetGuid& guid) {
    snprintf(out, out_size, "%016llx%016llx", static_cast<unsigned long long>(guid.hi), static_cast<unsigned long long>(guid.lo));
}

AssetResource* AssetResourceProvider::add(const AssetView& view, const char* path) {
    if (!view.is_parsed()) {
        fprintf(stderr, "[asset] error: cannot add %s: the view is not parsed\n", path != nullptr ? path : "(null)");
        return nullptr;
    }
    if (path == nullptr) {
        fprintf(stderr, "[asset] error: cannot add an asset without a path\n");
        return nullptr;
    }
    if (this->require_cooked && !view.is_cooked()) {
        fprintf(stderr, "[asset] error: %s is not cooked; the runtime only loads cooked assets\n", path);
        return nullptr;
    }

    const AssetGuid guid = view.header->guid;
    AssetResource* resource = this->find(guid);
    const bool known = resource != nullptr;
    if (!known) {
        resource = this->allocator->allocate_array<AssetResource>(1);
        new (resource) AssetResource();
        resource->guid = guid;
    }

    // The path: replaced only when it differs, so a rescan of an unchanged
    // project allocates nothing.
    if (resource->path == nullptr || std::strcmp(resource->path, path) != 0) {
        const usz length = std::strlen(path) + 1;
        char* copy = this->allocator->allocate_array<char>(length);
        std::memcpy(copy, path, length);
        this->allocator->free(const_cast<char*>(resource->path));
        resource->path = copy;
    }

    const bool same_version = known && resource->view.header->file_size == view.header->file_size &&
                              resource->view.header->content_hash == view.header->content_hash;
    if (!same_version) {
        // A different version of the file: whatever was read belongs to the
        // old chunk table.
        this->unload(resource);
        this->free_prelude(resource);
        if (!this->store_prelude(resource, view)) {
            // Cannot happen for a parsed view, but never keep a resource
            // without a prelude.
            this->release(resource);
            if (known) {
                this->resources.remove(guid);
            }
            return nullptr;
        }
    }

    if (!known) {
        this->resources.insert(guid, resource);
    }
    return resource;
}

AssetResource* AssetResourceProvider::find(const AssetGuid& guid) const {
    AssetResource* const* entry = this->resources.find(guid);
    return entry != nullptr ? *entry : nullptr;
}

bool AssetResourceProvider::remove(const AssetGuid& guid) {
    AssetResource* resource = this->find(guid);
    if (resource == nullptr) {
        return false;
    }
    this->release(resource);
    this->resources.remove(guid);
    return true;
}

bool AssetResourceProvider::store_prelude(AssetResource* resource, const AssetView& view) {
    u8* copy = static_cast<u8*>(this->allocator->allocate(view.size, ASSET_FILE::PAYLOAD_ALIGNMENT));
    std::memcpy(copy, view.data, view.size);
    if (resource->view.parse(copy, view.size) != ASSET_PARSE_OK) {
        this->allocator->free(copy);
        return false;
    }
    resource->prelude = copy;
    resource->prelude_size = view.size;

    const usz chunk_count = resource->view.chunk_count();
    resource->payloads = this->allocator->allocate_array<u8*>(chunk_count);
    if (chunk_count > 0) {
        std::memset(resource->payloads, 0, sizeof(u8*) * chunk_count);
    }
    return true;
}

void AssetResourceProvider::free_prelude(AssetResource* resource) {
    this->allocator->free(resource->payloads);
    this->allocator->free(resource->prelude);
    resource->payloads = nullptr;
    resource->prelude = nullptr;
    resource->prelude_size = 0;
    resource->view.reset();
}

void AssetResourceProvider::release(AssetResource* resource) {
    this->unload(resource);
    this->free_prelude(resource);
    this->allocator->free(const_cast<char*>(resource->path));
    resource->path = nullptr;
    this->allocator->free(resource);
}

// --- AssetResourceProvider: loading -----------------------------------------

AssetResource* AssetResourceProvider::get(const AssetGuid& guid) {
    AssetResource* resource = this->find(guid);
    if (resource == nullptr) {
        char text[40];
        print_guid(text, sizeof(text), guid);
        fprintf(stderr, "[asset] error: no asset with guid %s was added to the provider\n", text);
        return nullptr;
    }
    if (!resource->is_loaded() && !this->load(resource)) {
        return nullptr;
    }
    return resource;
}

AssetResource* AssetResourceProvider::get(const AssetView& view) {
    if (!view.is_parsed()) {
        fprintf(stderr, "[asset] error: get on a view that is not parsed\n");
        return nullptr;
    }
    return this->get(view.header->guid);
}

bool AssetResourceProvider::open(AssetResource* resource, AssetReader* reader, bool* out_refreshed) {
    *out_refreshed = false;
    if (!reader->open(resource->path)) {
        return false;
    }
    if (reader->matches(resource->view)) {
        return true;
    }
    if (reader->header.guid != resource->guid) {
        char text[40];
        print_guid(text, sizeof(text), resource->guid);
        fprintf(stderr, "[asset] error: %s no longer holds asset %s\n", resource->path, text);
        reader->close();
        return false;
    }

    // The file was re-imported since the prelude was taken: take it again
    // and drop payloads that belong to the old chunk table.
    usz size = 0;
    u8* prelude = reader->read_prelude(this->allocator, &size);
    if (prelude == nullptr) {
        reader->close();
        return false;
    }
    AssetView fresh;
    const AssetParseError error = fresh.parse(prelude, size);
    if (error != ASSET_PARSE_OK) {
        fprintf(stderr, "[asset] error: %s: %s\n", resource->path, ASSET_FILE::parse_error_name(error));
        this->allocator->free(prelude);
        reader->close();
        return false;
    }
    if (this->require_cooked && !fresh.is_cooked()) {
        fprintf(stderr, "[asset] error: %s is no longer cooked; the runtime only loads cooked assets\n", resource->path);
        this->allocator->free(prelude);
        reader->close();
        return false;
    }

    this->unload(resource);
    this->free_prelude(resource);
    // `prelude` already has the alignment store_prelude would give it; keep
    // it rather than copy.
    resource->prelude = prelude;
    resource->prelude_size = size;
    resource->view = fresh;
    const usz chunk_count = fresh.chunk_count();
    resource->payloads = this->allocator->allocate_array<u8*>(chunk_count);
    if (chunk_count > 0) {
        std::memset(resource->payloads, 0, sizeof(u8*) * chunk_count);
    }
    *out_refreshed = true;
    return true;
}

bool AssetResourceProvider::read_chunk(AssetResource* resource, AssetReader& reader, const usz chunk) {
    if (resource->is_resident(chunk)) {
        return true;
    }
    const ChunkEntry& entry = resource->view.chunks[chunk];
    u8* buffer = static_cast<u8*>(this->allocator->allocate(entry.size, ASSET_FILE::PAYLOAD_ALIGNMENT));
    if (buffer == nullptr) {
        fprintf(stderr, "[asset] error: out of memory loading a chunk of %s (%llu bytes)\n", resource->path,
                static_cast<unsigned long long>(entry.size));
        return false;
    }
    if (!reader.read_chunk(entry, buffer)) {
        this->allocator->free(buffer);
        return false;
    }
    resource->payloads[chunk] = buffer;
    resource->resident_bytes += entry.size;
    this->resident_total += entry.size;
    return true;
}

bool AssetResourceProvider::load(AssetResource* resource) {
    if (resource->is_loaded()) {
        return true;
    }
    AssetReader reader;
    bool refreshed = false;
    if (!this->open(resource, &reader, &refreshed)) {
        return false;
    }
    bool ok = true;
    const usz chunk_count = resource->view.chunk_count();
    for (usz i = 0; i < chunk_count && ok; ++i) {
        if (!resource->view.chunks[i].is_editor_only()) {
            ok = this->read_chunk(resource, reader, i);
        }
    }
    reader.close();
    return ok;
}

const u8* AssetResourceProvider::get_chunk_at(AssetResource* resource, const usz chunk) {
    if (chunk >= resource->view.chunk_count()) {
        return nullptr;
    }
    if (resource->is_resident(chunk)) {
        return resource->payloads[chunk];
    }
    AssetReader reader;
    bool refreshed = false;
    if (!this->open(resource, &reader, &refreshed)) {
        return nullptr;
    }
    const u8* result = nullptr;
    if (refreshed) {
        fprintf(stderr, "[asset] error: %s changed on disk; chunk %llu of the old table is gone, look it up again\n", resource->path,
                static_cast<unsigned long long>(chunk));
    } else if (this->read_chunk(resource, reader, chunk)) {
        result = resource->payloads[chunk];
    }
    reader.close();
    return result;
}

const u8* AssetResourceProvider::get_chunk(AssetResource* resource, const u32 tag, const ChunkEntry** out_chunk) {
    const ChunkEntry* entry = resource->view.find_chunk(tag);
    if (out_chunk != nullptr) {
        *out_chunk = entry;
    }
    if (entry == nullptr) {
        return nullptr;
    }
    usz chunk = static_cast<usz>(entry - resource->view.chunks);
    if (resource->is_resident(chunk)) {
        return resource->payloads[chunk];
    }

    AssetReader reader;
    bool refreshed = false;
    if (!this->open(resource, &reader, &refreshed)) {
        return nullptr;
    }
    if (refreshed) {
        entry = resource->view.find_chunk(tag);
        if (out_chunk != nullptr) {
            *out_chunk = entry;
        }
        if (entry == nullptr) {
            reader.close();
            return nullptr;
        }
        chunk = static_cast<usz>(entry - resource->view.chunks);
    }
    const u8* result = this->read_chunk(resource, reader, chunk) ? resource->payloads[chunk] : nullptr;
    reader.close();
    return result;
}

bool AssetResourceProvider::refresh(AssetResource* resource) {
    AssetReader reader;
    bool refreshed = false;
    const bool ok = this->open(resource, &reader, &refreshed);
    reader.close();
    return ok;
}

void AssetResourceProvider::unload(AssetResource* resource) {
    const usz chunk_count = resource->view.chunk_count();
    for (usz i = 0; i < chunk_count; ++i) {
        this->allocator->free(resource->payloads[i]);
        resource->payloads[i] = nullptr;
    }
    this->resident_total -= resource->resident_bytes;
    resource->resident_bytes = 0;
}

bool AssetResourceProvider::unload(const AssetGuid& guid) {
    AssetResource* resource = this->find(guid);
    if (resource == nullptr) {
        return false;
    }
    this->unload(resource);
    return true;
}

void AssetResourceProvider::unload_all() {
    for (auto& entry : this->resources) {
        this->unload(entry.value);
    }
}

void AssetResourceProvider::free() {
    for (auto& entry : this->resources) {
        this->release(entry.value);
    }
    this->resources.free();
    this->resident_total = 0;
}
