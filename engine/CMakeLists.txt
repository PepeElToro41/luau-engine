# --- Dependencies ------------------------------------------------------------

# volk and SDL3 are located by the top-level CMakeLists.txt (shared with
# vendor/imgui). volk provides the Vulkan headers and loads the loader at runtime.

# shaderc ships no CMake config, only headers + libraries. Build an imported
# target from the statically linked "combined" library (shaderc + glslang +
# SPIRV-Tools in one archive), searching the SDK first.
find_path(SHADERC_INCLUDE_DIR
    NAMES shaderc/shaderc.hpp
    HINTS "${VULKAN_SDK}/include"
)
find_library(SHADERC_LIBRARY
    NAMES shaderc_combined
    HINTS "${VULKAN_SDK}/lib"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(shaderc
    REQUIRED_VARS SHADERC_LIBRARY SHADERC_INCLUDE_DIR
)
mark_as_advanced(SHADERC_INCLUDE_DIR SHADERC_LIBRARY)

if(NOT TARGET shaderc::shaderc_combined)
    add_library(shaderc::shaderc_combined STATIC IMPORTED GLOBAL)
    set_target_properties(shaderc::shaderc_combined PROPERTIES
        IMPORTED_LOCATION "${SHADERC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SHADERC_INCLUDE_DIR}"
    )
endif()

# --- Target ------------------------------------------------------------------

file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
)

add_library(engine STATIC ${ENGINE_SOURCES} ${ENGINE_HEADERS})
add_library(LuauEngine::engine ALIAS engine)

target_include_directories(engine
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(engine PUBLIC cxx_std_20)

# All Vulkan entry points go through volk's function pointers, so the Vulkan
# headers must not declare the real prototypes anywhere in the engine or its
# consumers.
target_compile_definitions(engine PUBLIC VK_NO_PROTOTYPES)

# PUBLIC so standalone and editor inherit the headers and link flags.
target_link_libraries(engine
    PUBLIC
        volk::volk
        SDL3::SDL3
        imgui::imgui
    PRIVATE
        shaderc::shaderc_combined
)
