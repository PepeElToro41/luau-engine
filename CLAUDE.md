# LuauEngine

C++20 game engine built with CMake and Vulkan. Targets: `engine` (static library, `engine/`), `standalone` (`standalone/`), and `editor` (`editor/`).

## Build

```sh
cmake -S . -B build
cmake --build build
```

Binaries land in `build/bin`. `compile_commands.json` is exported to the build directory.

## C++ conventions

### Use `struct`, never `class`

Every type is declared with `struct`. Do not use the `class` keyword, even for types with private members. Use explicit `public:` / `private:` sections when access control is needed.

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

Member variables are plain names with no `m_` (or `_`, or trailing `_`) prefix. Inside member functions, always access members explicitly through `this->` so they are distinguishable from locals and parameters.

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

Functions, methods, member variables, locals, and parameters use `snake_case`. Type names stay `PascalCase`.

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

Do not declare destructors, not even defaulted or virtual ones. Cleanup is explicit: types that own resources expose a `shutdown()` (or similar) method that the owner calls. Because base types have no virtual destructor, never `delete` an object through a base pointer; destroy it as its concrete type.

The single exception is `TemporalAllocator` (`engine/include/engine/memory/temporal_allocator.hpp`): its destructor rewinds the thread's `MAIN_ARENA` to the mark taken at creation, which is the entire purpose of the type. Do not add others.

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

The engine targets Vulkan 1.1. Do not use features from later core versions:

- No dynamic rendering (`VK_KHR_dynamic_rendering` / `vkCmdBeginRendering`). Use `VkRenderPass` and `VkFramebuffer`.
- No timeline semaphores (`VK_KHR_timeline_semaphore`). Use binary semaphores and fences.
- No `synchronization2`; use the original `vkCmdPipelineBarrier` / `VkSubmitInfo` APIs.

Request `VK_API_VERSION_1_1` when creating the instance and require it when picking a physical device.

### Namespaces use SCREAMING_CASE

Namespaces group free functions and are named in `SCREAMING_CASE`. Functions inside keep `snake_case`.

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
