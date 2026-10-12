#include "engine/plugins/project_plugin.hpp"

#include "engine/engine.hpp"
#include "engine/plugins/core_plugin.hpp"
#include "engine/plugins/render_plugin.hpp"
#include "engine/project.hpp"

bool ProjectPlugin::build(Engine& engine) {
    if (engine.require<CorePlugin>() == nullptr || engine.require<RenderPlugin>() == nullptr) {
        return false;
    }
    if (!PROJECT::open(engine, this->root)) {
        return false;
    }
    engine.add_system(engine.phases.pre_startup, ProjectPlugin::NAME, &ProjectPlugin::scan, this);
    return true;
}

void ProjectPlugin::scan(SystemContext& ctx) {
    PROJECT::scan(ENGINE::of(ctx));
}
