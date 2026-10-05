# stb_image, vendored single header. Sourced from https://github.com/nothings/stb,
# stb_image.h v2.30 (public domain / MIT). The implementation is compiled once
# here, limited to the formats the editor imports, and without stdio: callers
# hand it bytes they read through the engine's platform layer.

add_library(stb_image STATIC stb_image.cpp stb_image.h)
add_library(stb::image ALIAS stb_image)

target_include_directories(stb_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
