#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"

#include <SDL3/SDL.h>

struct World;

// Editor-only tag on the camera the Viewport panel flies around. It exists
// only in the editor's World; the standalone never creates it.
struct EditorCamera {};

// The editor's own camera: an unparented entity (so never in the scene tree)
// named "editor_camera" with Transform + Camera + EditorCamera + RenderCamera.
// The renderer draws the Viewport from it and the scene's cameras are left as
// scene content.
//
// Controls: hold the right mouse button over the Viewport to fly. While held
// the cursor is hidden (SDL relative mouse mode) and the mouse turns the view
// (yaw about world up, pitch clamped short of straight up and down, never any
// roll), W / S move along the view direction, A / D sideways, E / Q up and
// down along world up, Shift multiplies the speed and the wheel scales it.
// Releasing the button ends fly mode and puts the cursor back where it was
// when the button went down.
//
// Fly mode is held on the button state alone. The window focus flag is not
// consulted: on some Wayland compositors it flaps when the pointer lock
// engages, and ending fly mode on it re-locked the pointer every frame,
// letting the hidden cursor drift between locks.
struct EditorCameraController {
    // Spawns the camera entity at a default pose looking at the origin.
    // False if the entity could not be created.
    bool init(World& world);

    // Reads SDL's input state and moves the camera for this frame. Call once
    // per frame before the engine renders. `viewport_hovered` is whether the
    // mouse was over the Viewport image when the UI was last drawn: fly mode
    // only begins there, and holds until the button goes up. `window` is
    // where relative mouse mode is switched.
    void update(World& world, SDL_Window* window, bool viewport_hovered, f32 dt);
    // Wheel motion from the event loop (SDL has no wheel state to poll); it is
    // applied to the speed on the next update while flying, dropped otherwise.
    void add_wheel(f32 delta) { this->wheel += delta; }

    EntityId entity = 0;
    bool flying = false;
    // Units per second; Shift multiplies it; the wheel scales it by steps.
    f32 speed = 4.0f;
    f32 fast_multiplier = 4.0f;
    // Radians per mouse pixel.
    f32 look_sensitivity = 0.003f;

private:
    void begin_fly(World& world, SDL_Window* window);
    void end_fly(SDL_Window* window);

    // The view angles while flying, taken from the Transform when fly mode
    // begins so a pose set elsewhere is respected, then integrated here so
    // the quaternion round trip does not drift.
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;
    f32 wheel = 0.0f;
    // Window-relative cursor position when fly mode began; the cursor is
    // warped back there when it ends.
    f32 restore_x = 0.0f;
    f32 restore_y = 0.0f;
};
