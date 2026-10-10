#include "editor_camera.hpp"

#include "engine/ecs/world.hpp"
#include "engine/math/math.hpp"
#include "engine/render/components.hpp"
#include "engine/scene/scene.hpp"

#include <cmath>

static constexpr f32 PITCH_LIMIT = MATH::PI * 0.5f - 0.01f;
static constexpr f32 MIN_SPEED = 0.05f;
static constexpr f32 MAX_SPEED = 500.0f;

// The yaw / pitch whose from_euler(pitch, yaw, 0) rotation turns
// Vector3::forward() onto unit `direction`.
static void look_angles(const Vector3 direction, f32* yaw, f32* pitch) {
    *pitch = std::asin(MATH::clamp(direction.y, -1.0f, 1.0f));
    *yaw = std::atan2(-direction.x, -direction.z);
}

bool EditorCameraController::init(World& world) {
    this->entity = SCENE::spawn(world, "editor_camera", 0);
    if (this->entity == 0) {
        return false;
    }
    Transform transform;
    transform.position = Vector3(0.0f, 1.5f, 3.0f);
    look_angles((Vector3::zero() - transform.position).normalized(), &this->yaw, &this->pitch);
    transform.rotation = Quaternion::from_euler(this->pitch, this->yaw, 0.0f);
    world.set(this->entity, transform);
    world.set(this->entity, Camera{});
    world.add<EditorCamera>(this->entity);
    world.add<RenderCamera>(this->entity);
    return true;
}

void EditorCameraController::begin_fly(World& world, SDL_Window* window) {
    const Transform* transform = world.get<Transform>(this->entity);
    if (transform == nullptr) {
        return;
    }
    const Vector3 euler = transform->rotation.to_euler();
    this->pitch = MATH::clamp(euler.x, -PITCH_LIMIT, PITCH_LIMIT);
    this->yaw = euler.y;
    this->wheel = 0.0f;
    SDL_GetMouseState(&this->restore_x, &this->restore_y);
    SDL_SetWindowRelativeMouseMode(window, true);
    // Drop the motion accumulated before fly mode began.
    SDL_GetRelativeMouseState(nullptr, nullptr);
    this->flying = true;
}

void EditorCameraController::end_fly(SDL_Window* window) {
    // Warp before leaving relative mode: on Wayland a warp under a pointer
    // lock is a position hint the compositor applies when the lock lifts,
    // which is the only way to place the cursor there; elsewhere it is a
    // plain warp that relative mode keeps from producing a motion event.
    SDL_WarpMouseInWindow(window, this->restore_x, this->restore_y);
    SDL_SetWindowRelativeMouseMode(window, false);
    this->flying = false;
}

void EditorCameraController::update(World& world, SDL_Window* window, const bool viewport_hovered, const f32 dt) {
    if (this->entity == 0 || window == nullptr) {
        return;
    }
    const bool right_down = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK) != 0;

    if (!this->flying) {
        this->wheel = 0.0f;
        if (right_down && viewport_hovered) {
            this->begin_fly(world, window);
        }
        return;
    }
    if (!right_down) {
        this->end_fly(window);
        return;
    }
    Transform* transform = world.get<Transform>(this->entity);
    if (transform == nullptr) {
        this->end_fly(window);
        return;
    }

    // Look: mouse right turns right (yaw decreases, +yaw turns toward -X),
    // mouse down looks down.
    f32 dx = 0.0f, dy = 0.0f;
    SDL_GetRelativeMouseState(&dx, &dy);
    this->yaw -= dx * this->look_sensitivity;
    this->pitch = MATH::clamp(this->pitch - dy * this->look_sensitivity, -PITCH_LIMIT, PITCH_LIMIT);
    this->yaw = std::remainder(this->yaw, 2.0f * MATH::PI);
    const Quaternion rotation = Quaternion::from_euler(this->pitch, this->yaw, 0.0f);

    // Speed: one wheel notch is a 25% step.
    if (this->wheel != 0.0f) {
        this->speed = MATH::clamp(this->speed * std::pow(1.25f, this->wheel), MIN_SPEED, MAX_SPEED);
        this->wheel = 0.0f;
    }

    // Move: W / S along the view, A / D sideways, E / Q along world up.
    const bool* keys = SDL_GetKeyboardState(nullptr);
    Vector3 move = Vector3::zero();
    if (keys[SDL_SCANCODE_W]) {
        move += rotation * Vector3::forward();
    }
    if (keys[SDL_SCANCODE_S]) {
        move -= rotation * Vector3::forward();
    }
    if (keys[SDL_SCANCODE_D]) {
        move += rotation * Vector3::right();
    }
    if (keys[SDL_SCANCODE_A]) {
        move -= rotation * Vector3::right();
    }
    if (keys[SDL_SCANCODE_E]) {
        move += Vector3::up();
    }
    if (keys[SDL_SCANCODE_Q]) {
        move -= Vector3::up();
    }
    const bool fast = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    if (move.length_squared() > 0.0f) {
        move = move.normalized() * (this->speed * (fast ? this->fast_multiplier : 1.0f) * dt);
    }

    transform->rotation = rotation;
    transform->position += move;
    world.modified<Transform>(this->entity);
}
