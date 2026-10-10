#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"

// Shaders as entities. A shader is found by name: <project render
// dir>/<name>.slang (then .glsl) first, then the same under the engine
// render dir; both directories are include roots. Loading the same name
// twice returns the same entity. The entity is named but unparented, so it
// never shows up in the scene tree.

struct Renderer;

namespace SHADER_LIBRARY {

// Loads <name>.slang (or .glsl) into a new entity with a Shader component.
// 0 on failure, with the compiler's log sent to the renderer's log.
EntityId load(Renderer& renderer, const char* name);
// Recompiles the entity's file. On success the new program replaces the
// old one in place, its pipelines are released and `generation` bumps; on
// failure the old program keeps drawing and the log is reported.
bool reload(Renderer& renderer, EntityId shader);
// Reloads every Shader entity.
void reload_all(Renderer& renderer);

} // namespace SHADER_LIBRARY
