# LuauEngine

C++20 game engine built with CMake and Vulkan.

## Layout

The engine lives in `engine/` as three static libraries plus an umbrella target.
Each module is described in a `CLAUDE.md` next to its sources, imported here so every session reads all of them:

@engine/core/CLAUDE.md
@engine/graphics/CLAUDE.md
@engine/runtime/CLAUDE.md
@editor/CLAUDE.md

Each module has `include/engine/...` (public) and `src/...`;
sources are globbed, so new files need no CMake edit.
Includes always use the `engine/` prefix regardless of module.

Vendored third-party code is copied under `vendor/<lib>/` with a hand-written `CMakeLists.txt` (imgui, doctest, nanobench, stb: `stb_image.h` compiled once in `stb_image.cpp` as `stb::image`, PNG/JPEG/BMP/TGA only, no stdio;
spirv-reflect: `spirv_reflect.c/.h` from the Vulkan SDK as `spirv_reflect::spirv_reflect`, linked only by graphics).

Shaders live in the repo's `render/` directory (`ENGINE_RENDER_DIR`),
written in Slang, one `.slang` file per shader: `#pragma pass <tag> [vertex] [fragment]` lines name the render passes it draws in, and the stages are the `[shader("vertex")] ... vertex(...)` and `[shader("fragment")] ... fragment(...)` functions of that same file, with uniforms declared once above them and the varyings as the struct passed from one to the other (`SV_Position` for the clip position).
Each pass compiles those functions with `PASS_<TAG>` and `STAGE_*` defined, so `#ifdef PASS_SHADOW` specializes code per pass and a vertex-only pass simply never asks for `fragment`.
`render/engine/*.slang` is the include library: `frame.slang` (set 0: the `frame` block at binding 0 and the engine sampler table `sampler_linear` / `sampler_linear_clamp` / `sampler_nearest` / `sampler_nearest_clamp` at bindings 1..4;
a shader's set 0 must be a subset of it),
`object.slang` (push constants `object`),
`bindings.slang` (`MATERIAL(b)` = set 2 binding b, `PASS_INPUT(i)` = set 1 binding i;
textures are `Texture2D`, sampled with a frame sampler or a material `SamplerState`, never `Sampler2D`, which D3D12 and Metal cannot express),
`vertex.slang` (`VERTEX_POSITION` ...
`VERTEX_WEIGHTS`: the `[[vk::location(N)]]` of each `VERTEX_LAYOUT` semantic, required because Slang numbers inputs in declaration order;
declare only what the shader reads),
`fullscreen.slang` (the fullscreen-triangle `vertex` function and its `FullscreenVertex`).
Matrices in blocks are column-major `Matrix4x4`;
`mul(m, v)` is GLSL's `m * v`.
A project's own `render/` directory overrides files by name.
`.glsl` shaders (one `main` per `#ifdef STAGE_*` section, `layout(set, binding)` by hand) still load through the shaderc backend.
Slang's `SV_VertexID` is relative to the base vertex, which is why the device enables `shaderDrawParameters`.

## Editing these docs

One sentence per line in every paragraph, here and in the module files, and a long sentence is broken further after each `;` and after each `),` outside a code span.
Markdown joins the lines back into a paragraph, and git then diffs and merges by line, so two branches that add to the same paragraph no longer conflict.
Never rewrap a paragraph: add a new line for a new clause, or change the line that holds the one you mean.
A module's description lives in its own file, so a change to core belongs in `engine/core/CLAUDE.md`, graphics in `engine/graphics/CLAUDE.md`, runtime in `engine/runtime/CLAUDE.md`, the apps in `editor/CLAUDE.md`; this file keeps only the build, test and convention sections.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Binaries land in `build/bin`.
`compile_commands.json` is exported to the build directory.

CMake options: `LUAU_ENGINE_BUILD_TESTS` (ON),
`LUAU_ENGINE_BUILD_BENCHES` (ON),
`LUAU_ENGINE_SANITIZE` (OFF, adds ASan + UBSan to every target;
use a separate build dir).

## Tests

doctest, one binary `engine_tests` built from every `.cpp` under `tests/`.

```sh
cmake --build build --target engine_tests && ./build/bin/engine_tests   # all tests
./build/bin/engine_tests -tc="ecs/world*"                               # filter by test-case name (wildcards)
./build/bin/engine_tests -sf="*entity_index*"                           # filter by source file
./build/bin/engine_tests -s                                             # also print passing assertions
ctest --test-dir build --output-on-failure                              # same, via CTest (one entry per TEST_CASE)

cmake -S . -B build-asan -DLUAU_ENGINE_SANITIZE=ON
cmake --build build-asan --target engine_tests && ./build-asan/bin/engine_tests
```

Conventions:

- `tests/core/<dir>/<unit>_tests.cpp` mirrors `engine/core/{include,src}/engine/<dir>/<unit>`;
  `tests/graphics/shaders/preprocessing_tests.cpp` is the one test outside core, covering the graphics source `engine_tests` compiles in directly (see `tests/CMakeLists.txt`).
  Nothing else from graphics goes in: the binary must build without SDL3 / Vulkan.
- Start every file with `#include "support/test_support.hpp"` (doctest, sample component types `Position`/`Velocity`/`Health`/`TagA`/`TagB`/`Likes`/`Eats`, `CHECK_ARENA_CLEAN()`, `HookLog`).
- `TEST_CASE` names are `"<area>/<unit>: <behaviour>"`, e.g. `"ecs/world: delete_entity bumps the generation"`, so `-tc="ecs/world*"` selects a unit.
  Use `SUBCASE` for variants that share setup.
- No RAII fixtures (the no-destructor rule applies to tests).
  World tests are explicit: `World world; world.init(); ... world.free(); CHECK_ARENA_CLEAN();`.
  Free every container you create.
- `ENGINE_ASSERT` aborts, so assertion paths are not tested;
  test documented return values instead.
  Never assert exact `TypeId` values.
- A test that exposes a suspected engine bug keeps the documented behaviour, is decorated `* doctest::may_fail()` and carries an `// ENGINE BUG?:` comment, rather than being bent to pass.

## Benchmarks

nanobench, one binary `engine_bench` built from every `.cpp` under `bench/`.
Build it in Release;
a Debug build runs but warns, since `ENGINE_ASSERT` and `-O0` skew the numbers.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target engine_bench && ./build-release/bin/engine_bench   # all
./build-release/bin/engine_bench ecs/component hash_map                                 # names containing any argument
./build-release/bin/engine_bench --list                                                 # names only
```

Adding a benchmark is one `BENCH_CASE("<area>/<unit>: <what>") { ... }` block in a `bench/core/<dir>/<unit>_bench.cpp` file (see `bench/support/bench.hpp`);
it self-registers, nothing else changes.
`bench` is a configured `ankerl::nanobench::Bench`;
call `bench.run("label", [&] { ... })` one or more times.
Use `set<T>` (not `add<T>`) for data components, wrap results in `ankerl::nanobench::doNotOptimizeAway`, and use `bench.batch(n)` when a run does `n` operations so the table shows per-operation cost.
When every iteration needs a fresh index into a pre-built pool (an entity that has not had the id yet),
use `BENCH::run_indexed(bench, "label", count, [&](usz i) { ... })`: it calls the lambda exactly `count` times with `i` from 0 to `count - 1` (see `bench/core/ecs/fresh_entity_bench.cpp`).

## C++ conventions

### Use `struct`, never `class`

Every type is declared with `struct`.
Do not use the `class` keyword, even for types with private members.
Use explicit `public:` / `private:` sections when access control is needed.

```cpp
// Good
struct Engine {
    void init();

private:
    VkInstance instance = VK_NULL_HANDLE;
};

// Bad
class Engine { ... };
```

### No `m_` prefix; access members through `this->`

Member variables are plain names with no `m_` (or `_`, or trailing `_`) prefix.
Inside member functions, always access members explicitly through `this->` so they are distinguishable from locals and parameters.

```cpp
// Good
struct Engine {
    void init();

private:
    VkInstance instance = VK_NULL_HANDLE;
};

void Engine::init() {
    VkResult result = vkCreateInstance(&createInfo, nullptr, &this->instance);
    if (result != VK_SUCCESS) {
        this->instance = VK_NULL_HANDLE;
    }
}

// Bad
class Engine {
    VkInstance m_instance = VK_NULL_HANDLE;
};

void Engine::init() {
    vkCreateInstance(&createInfo, nullptr, &m_instance);
}
```

### snake_case for functions and variables

Functions, methods, member variables, locals, and parameters use `snake_case`.
Type names stay `PascalCase`.

```cpp
// Good
struct VulkanBackend {
    void begin_frame();

private:
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
};

// Bad
struct VulkanBackend {
    void beginFrame();

private:
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
};
```

### No destructors

Do not declare destructors, not even defaulted or virtual ones.
Cleanup is explicit: types that own resources expose a `shutdown()` (or similar) method that the owner calls.
Because base types have no virtual destructor, never `delete` an object through a base pointer;
destroy it as its concrete type.

The single exception is `TemporalAllocator` (`engine/include/engine/memory/temporal_allocator.hpp`): its destructor rewinds the thread's `MAIN_ARENA` to the mark taken at creation, which is the entire purpose of the type.
Do not add others.

```cpp
// Good
struct VulkanBackend : RendererBackend {
    void init() override;
    void shutdown() override;
};

// Bad
struct VulkanBackend : RendererBackend {
    ~VulkanBackend() override { this->shutdown(); }
};
```

## Vulkan target: 1.1

The engine targets Vulkan 1.1.
Do not use features from later core versions:

- No dynamic rendering (`VK_KHR_dynamic_rendering` / `vkCmdBeginRendering`).
  Use `VkRenderPass` and `VkFramebuffer`.
- No timeline semaphores (`VK_KHR_timeline_semaphore`).
  Use binary semaphores and fences.
- No `synchronization2`;
  use the original `vkCmdPipelineBarrier` / `VkSubmitInfo` APIs.

Request `VK_API_VERSION_1_1` when creating the instance and require it when picking a physical device.
The one feature enabled beyond core 1.0 is `shaderDrawParameters` (core in 1.1, via `VkPhysicalDeviceVulkan11Features`),
which Slang's `SV_VertexID` needs.

### Namespaces use SCREAMING_CASE

Namespaces group free functions and are named in `SCREAMING_CASE`.
Functions inside keep `snake_case`.

```cpp
// Good
namespace DISPLAY_WINDOW {
bool sdl_initialize();
void sdl_shutdown();
}

// Bad
namespace display_window { ... }
namespace DisplayWindow { ... }
```
