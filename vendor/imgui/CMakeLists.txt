# Dear ImGui (docking branch), vendored. Only the SDL3 and Vulkan backends are
# included. Sourced from https://github.com/ocornut/imgui, tag v1.92.9b-docking.

add_library(imgui STATIC
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_sdl3.cpp
    backends/imgui_impl_vulkan.cpp
)
add_library(imgui::imgui ALIAS imgui)

target_include_directories(imgui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

target_compile_features(imgui PUBLIC cxx_std_20)

# The engine routes every Vulkan call through volk (VK_NO_PROTOTYPES), so the
# ImGui Vulkan backend must do the same instead of calling the loader directly.
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)

target_link_libraries(imgui
    PUBLIC
        Vulkan::volk
        SDL3::SDL3
)
