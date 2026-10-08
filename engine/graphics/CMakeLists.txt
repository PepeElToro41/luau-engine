# engine_graphics: the display window and the renderer backends. Owns every
# graphics / platform dependency (volk, SDL3, shaderc, Slang, imgui).

# --- Dependencies ------------------------------------------------------------

# volk, shaderc and SDL3 are located by the top-level CMakeLists.txt (volk and
# SDL3 are shared with vendor/imgui). volk provides the Vulkan headers and loads
# the loader at runtime; shaderc_combined is the statically linked shaderc +
# glslang + SPIRV-Tools archive.

# Slang, the engine's shader language. The SDK ships it as a shared library
# (libslang.so, which loads its helper libraries from its own directory at
# runtime) with the headers under include/slang/.
include(FindPackageHandleStandardArgs)
find_path(SLANG_INCLUDE_DIR
    NAMES slang/slang.h
    HINTS "${VULKAN_SDK}/include"
)
find_library(SLANG_LIBRARY
    NAMES slang
    HINTS "${VULKAN_SDK}/lib"
)
find_package_handle_standard_args(slang
    REQUIRED_VARS SLANG_LIBRARY SLANG_INCLUDE_DIR
)
mark_as_advanced(SLANG_INCLUDE_DIR SLANG_LIBRARY)

if(NOT TARGET slang::slang)
    add_library(slang::slang SHARED IMPORTED GLOBAL)
    set_target_properties(slang::slang PROPERTIES
        IMPORTED_LOCATION "${SLANG_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIR}"
    )
endif()

# --- Target ------------------------------------------------------------------

file(GLOB_RECURSE GRAPHICS_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)
file(GLOB_RECURSE GRAPHICS_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
)

add_library(engine_graphics STATIC ${GRAPHICS_SOURCES} ${GRAPHICS_HEADERS})
add_library(LuauEngine::graphics ALIAS engine_graphics)

target_include_directories(engine_graphics
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(engine_graphics PUBLIC cxx_std_20)

# All Vulkan entry points go through volk's function pointers, so the Vulkan
# headers must not declare the real prototypes anywhere in the engine or its
# consumers.
target_compile_definitions(engine_graphics PUBLIC VK_NO_PROTOTYPES)

# PUBLIC so runtime, standalone and editor inherit the headers and link flags.
target_link_libraries(engine_graphics
    PUBLIC
        LuauEngine::core
        Vulkan::volk
        SDL3::SDL3
        imgui::imgui
    PRIVATE
        Vulkan::shaderc_combined
        slang::slang
        spirv_reflect::spirv_reflect
)
