# SPIRV-Reflect, vendored from the LunarG Vulkan SDK 1.4.363.0
# (https://github.com/KhronosGroup/SPIRV-Reflect, Apache 2.0). Two files plus
# the SPIR-V header it parses with. The engine uses it to read descriptor
# bindings, push constants and vertex inputs out of compiled shaders; only
# engine/graphics links it, and only src/gpu/shader_reflect.cpp includes it.

enable_language(C)

add_library(spirv_reflect STATIC spirv_reflect.c spirv_reflect.h)
add_library(spirv_reflect::spirv_reflect ALIAS spirv_reflect)

target_include_directories(spirv_reflect PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
