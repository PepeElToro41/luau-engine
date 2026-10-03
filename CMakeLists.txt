cmake_minimum_required(VERSION 3.20)

project(LuauEngine
    VERSION 0.1.0
    LANGUAGES CXX
)

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

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# volk and SDL3 are shared by the vendored ImGui and the engine.
find_package(volk CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)

add_subdirectory(vendor/imgui)
add_subdirectory(engine)
add_subdirectory(standalone)
add_subdirectory(editor)
