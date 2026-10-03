# The engine is split into modules, each a static library:
#
#   core      defines, memory, templates, utils, ecs. No external dependencies,
#             so tests and benchmarks link only this.
#   graphics  window + renderer backends. Owns the SDL3 / volk / shaderc / imgui
#             dependencies.
#   runtime   the Engine application object (and later physics etc).
#
# LuauEngine::engine is an interface target that pulls in all of them, which is
# what standalone and editor link.

add_subdirectory(core)
add_subdirectory(graphics)
add_subdirectory(runtime)

add_library(engine INTERFACE)
add_library(LuauEngine::engine ALIAS engine)

target_link_libraries(engine
    INTERFACE
        LuauEngine::core
        LuauEngine::graphics
        LuauEngine::runtime
)
