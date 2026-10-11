#include "app.hpp"

#include <cstdio>
#include <cstring>

int main(const int argc, char** argv) {
    App app;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--post") == 0) {
            app.demo_post = true;
        } else if (strcmp(argv[i], "--shadow") == 0) {
            app.demo_shadow = true;
        } else if (strcmp(argv[i], "--cooked") == 0) {
            app.cooked_only = true;
        } else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            app.project_dir = argv[++i];
        } else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            app.scene_path = argv[++i];
        } else {
            fprintf(stderr, "usage: standalone [--project <dir>] [--scene <path>] [--post] [--shadow] [--cooked]\n");
            return 1;
        }
    }
    const bool ok = app.init();
    if (ok) {
        app.run();
    }
    app.shutdown();
    return ok ? 0 : 1;
}
