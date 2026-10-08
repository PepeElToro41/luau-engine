#include "app.hpp"

#include <cstring>

int main(const int argc, char** argv) {
    App app;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--post") == 0) {
            app.demo_post = true;
        } else if (strcmp(argv[i], "--shadow") == 0) {
            app.demo_shadow = true;
        }
    }
    const bool ok = app.init();
    if (ok) {
        app.run();
    }
    app.shutdown();
    return ok ? 0 : 1;
}
