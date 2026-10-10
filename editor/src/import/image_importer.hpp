#pragma once

#include "engine/asset/asset_types/texture_asset.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "import/importer.hpp"

#include <filesystem>
#include <string>

// Image import through stb_image (vendor/stb): PNG first, and the other
// formats stb decodes with the same call (JPEG, BMP, TGA). IMAGE::decode turns
// the file's bytes into a DecodedImage, a 2D mip 0 in one of the engine's
// uncompressed formats; DecodedImage::source describes it as the
// TextureSource the core's TextureAssetWriter builds from, and IMAGE::import
// does the whole file-to-.lunaasset trip.
//
//     TextureImportOptions options;
//     options.srgb = true; // color data
//     std::string error;
//     if (!IMAGE::import(ImportSource::file("wall.png"), "wall.lunaasset", options, IMPORT::random_guid(), &error)) {
//         output.error("%s", error.c_str());
//     }
//
// Channel mapping: 1 channel -> R8 / R16, 2 -> RG8 / RG16, 3 and 4 -> RGBA8 /
// RGBA16 (there is no RGB format; three-channel images get an opaque alpha).
// 16-bit PNGs stay 16-bit UNORM; everything else is 8-bit UNORM. Whether the
// data is sRGB is not in the file in any reliable way, so it is the import
// option's call: the writer picks the sRGB variant of the format when asked.

struct DecodedImage {
    u32 width = 0;
    u32 height = 0;
    u32 channels = 0;                 // after mapping: 1, 2 or 4
    u32 format = TEXTURE_FORMAT_NONE; // TextureFormat
    u8* pixels = nullptr;             // owned by stb_image; free() releases it
    usz pixels_size = 0;              // width * height * channels * bytes per channel

    // The image as the TextureAssetWriter takes it: a 2D texture, one layer.
    TextureSource source() const;

    bool is_decoded() const { return this->pixels != nullptr; }
    void free();
};

namespace IMAGE {

// Whether `extension` (lowercase, with the dot: ".png") is a format decode()
// reads.
bool is_supported_extension(const char* extension);

// Decodes an image file's bytes into `out`, which is freed first. False with
// `error` set (stb_image's reason) when the bytes are not a supported image.
bool decode(const void* bytes, usz size, DecodedImage* out, std::string* error);

// Reads the bytes of `source` (a file, or a kept original inside an asset:
// importer.hpp), decodes them, builds the texture payloads with `options`
// and writes a texture asset with `guid` to `destination`, keeping the
// bytes in its SRC chunk when `options.keep_source` is set. The bytes are
// read whole before anything is written, so `destination` may be the asset
// `source` points into. False with `error` set (and no file written) on
// any failure.
bool import(const ImportSource& source, const std::filesystem::path& destination, const TextureImportOptions& options,
            const AssetGuid& guid, std::string* error);

} // namespace IMAGE
