// The one translation unit that holds stb_image's implementation. The header
// is included everywhere else without STB_IMAGE_IMPLEMENTATION.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include "stb_image.h"
