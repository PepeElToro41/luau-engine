#include "import/image_importer.hpp"

#include "import/importer.hpp"

#include "engine/asset/asset_writer.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <stb_image.h>
#include <cstring>

// --- DecodedImage ------------------------------------------------------------------------

TextureSource DecodedImage::source() const {
    TextureSource source;
    source.width = this->width;
    source.height = this->height;
    source.depth = 1;
    source.layers = 1;
    source.format = this->format;
    source.dimension = TEXTURE_DIMENSION_2D;
    source.pixels = this->pixels;
    source.pixels_size = this->pixels_size;
    return source;
}

void DecodedImage::free() {
    stbi_image_free(this->pixels);
    *this = DecodedImage{};
}

// --- Decoding ------------------------------------------------------------------------------

bool IMAGE::is_supported_extension(const char* extension) {
    static const char* const supported[] = {".png", ".jpg", ".jpeg", ".bmp", ".tga"};
    for (const char* candidate : supported) {
        if (strcmp(extension, candidate) == 0) {
            return true;
        }
    }
    return false;
}

namespace {

// Three channels become four: the engine has no RGB format and GPUs rarely
// want one. One and two channels are kept as they are.
u32 mapped_channels(const int channels_in_file) {
    return channels_in_file == 3 ? 4 : static_cast<u32>(channels_in_file);
}

u32 format_for(const u32 channels, const bool sixteen_bit) {
    switch (channels) {
	    case 1: return sixteen_bit ? TEXTURE_FORMAT_R16_UNORM : TEXTURE_FORMAT_R8_UNORM;
	    case 2: return sixteen_bit ? TEXTURE_FORMAT_RG16_UNORM : TEXTURE_FORMAT_RG8_UNORM;
	    case 4: return sixteen_bit ? TEXTURE_FORMAT_RGBA16_UNORM : TEXTURE_FORMAT_RGBA8_UNORM;
	    default: return TEXTURE_FORMAT_NONE;
    }
}

} // namespace

bool IMAGE::decode(const void* bytes, const usz size, DecodedImage* out, std::string* error) {
    out->free();
    if (bytes == nullptr || size == 0 || size > 0x7fffffffull) {
        *error = "the image is empty or larger than stb_image can take";
        return false;
    }
    const stbi_uc* data = static_cast<const stbi_uc*>(bytes);
    const int length = static_cast<int>(size);

    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    if (!stbi_info_from_memory(data, length, &width, &height, &channels_in_file)) {
        *error = stbi_failure_reason();
        return false;
    }
    const u32 channels = mapped_channels(channels_in_file);
    const bool sixteen_bit = stbi_is_16_bit_from_memory(data, length) != 0;

    void* pixels = nullptr;
    if (sixteen_bit) {
        pixels = stbi_load_16_from_memory(data, length, &width, &height, &channels_in_file, static_cast<int>(channels));
    } else {
        pixels = stbi_load_from_memory(data, length, &width, &height, &channels_in_file, static_cast<int>(channels));
    }
    if (pixels == nullptr) {
        *error = stbi_failure_reason();
        return false;
    }

    out->width = static_cast<u32>(width);
    out->height = static_cast<u32>(height);
    out->channels = channels;
    out->format = format_for(channels, sixteen_bit);
    out->pixels = static_cast<u8*>(pixels);
    out->pixels_size = static_cast<usz>(width) * static_cast<usz>(height) * channels * (sixteen_bit ? 2 : 1);
    return true;
}

// --- Import --------------------------------------------------------------------------------

bool IMAGE::import(
	const ImportSource& source, 
	const std::filesystem::path& destination, 
	const TextureImportOptions& options,
	const AssetGuid& guid, std::string* error
) {
    const std::string source_path = source.describe();
    usz size = 0;
    u8* bytes = IMPORT::read_source(source, MEMORY::heap_allocator(), &size, error);
    if (bytes == nullptr) {
        return false;
    }

    DecodedImage image;
    if (!IMAGE::decode(bytes, size, &image, error)) {
        *error = source_path + ": " + *error;
        MEMORY::heap_allocator()->free(bytes);
        return false;
    }

    TextureAssetWriter payloads;
    const TextureWriteError write_error = payloads.build(image.source(), options);
    if (write_error != TEXTURE_WRITE_OK) {
        *error = source_path + ": " + TEXTURE_ASSET::write_error_name(write_error);
        payloads.free();
        image.free();
        MEMORY::heap_allocator()->free(bytes);
        return false;
    }

    AssetWriter writer;
    writer.type = ASSET_TYPE::TEXTURE;
    writer.guid = guid;
    writer.content_hash = IMPORT::fnv1a(bytes, size);
    bool ok = IMPORT::add_editor_chunks(writer, source, bytes, size, options.keep_source, error);
    if (ok) {
        payloads.add_chunks(writer);
        ok = IMPORT::write_asset(writer, destination.string().c_str(), error);
    }

    writer.free();
    payloads.free();
    image.free();
    MEMORY::heap_allocator()->free(bytes);
    return ok;
}
