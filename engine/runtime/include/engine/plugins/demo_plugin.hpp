#pragma once

#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

struct Engine;

// The development toggles both apps offer (render/demo_scene.hpp): the
// post-process pass and the depth-only shadow pass as state, applied in
// post_startup from the constructor's flags (`--post` / `--shadow`), and
// the keys in the events phase when a WindowPlugin is there: F5 reloads
// every shader, F6 toggles the post pass, F7 the shadow pass. An editor
// menu reads and writes the same state through set_post / set_shadow.
// Requires RenderPlugin.
struct DemoPlugin : Plugin {
    static constexpr const char* NAME = "demo";

    DemoPlugin() = default;
    DemoPlugin(const bool post, const bool shadow) : post(post), shadow(shadow) {}

    bool build(Engine& engine) override;

    // False (logged by the renderer) when the pass's shader did not load;
    // the flag then stays as it was.
    bool set_post(Engine& engine, bool enabled);
    bool set_shadow(Engine& engine, bool enabled);
    void reload_shaders(Engine& engine);

    bool post = false;
    bool shadow = false;

private:
    static void apply(SystemContext& ctx);
    static void handle_keys(SystemContext& ctx);
};
