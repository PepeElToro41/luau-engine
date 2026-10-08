cmake_minimum_required(VERSION 3.25)

project(LuauEngine
    VERSION 0.1.0
    LANGUAGES CXX
)

# The Windows Vulkan SDK ships MSVC-built libraries (shaderc_combined, volk);
# MinGW's linker cannot use them and stalls instead of failing.
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "On Windows, configure with the Visual Studio toolchain (cl.exe or clang-cl). "
        "The Vulkan SDK libraries are MSVC builds and do not link with MinGW.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Locate the LunarG Vulkan SDK. Taken from -DVULKAN_SDK=<path> or the
# VULKAN_SDK environment variable (set by the SDK's setup-env.sh). IDEs such as
# CLion do not source the shell profile, so pass it explicitly there.
set(VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "Root of the LunarG Vulkan SDK (the x86_64 directory)")
if(VULKAN_SDK)
    list(PREPEND CMAKE_PREFIX_PATH "${VULKAN_SDK}" "${VULKAN_SDK}/lib/VulkanLoader")
    set(ENV{VULKAN_SDK} "${VULKAN_SDK}")
else()
    message(WARNING "VULKAN_SDK is not set; volk and shaderc will only be found if installed system-wide.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(LUAU_ENGINE_BUILD_TESTS   "Build the engine_tests executable (doctest)"   ON)
option(LUAU_ENGINE_BUILD_BENCHES "Build the engine_bench executable (nanobench)" ON)
option(LUAU_ENGINE_SANITIZE      "Build every target with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# Global on purpose: the point is to instrument the engine libraries themselves,
# not just the test binary. Use a separate build directory (e.g. build-asan).
if(LUAU_ENGINE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# volk and SDL3 are shared by the vendored ImGui and the engine.
# volk and shaderc come from the SDK through CMake's FindVulkan, which picks the
# debug-CRT libraries (volkd, shaderc_combinedd) for Debug builds on Windows.
find_package(Vulkan REQUIRED COMPONENTS volk shaderc_combined)
# The Windows SDK keeps volk.h under Include/Volk; the Linux SDK has it in include/.
if(EXISTS "${Vulkan_INCLUDE_DIR}/Volk/volk.h")
    set_property(TARGET Vulkan::volk APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}/Volk")
endif()
find_package(SDL3 CONFIG REQUIRED)

add_subdirectory(vendor/imgui)
add_subdirectory(vendor/stb)
add_subdirectory(vendor/spirv-reflect)
add_subdirectory(engine)
add_subdirectory(standalone)
add_subdirectory(editor)

if(LUAU_ENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(vendor/doctest)
    add_subdirectory(tests)
endif()

if(LUAU_ENGINE_BUILD_BENCHES)
    add_subdirectory(vendor/nanobench)
    add_subdirectory(bench)
endif()
