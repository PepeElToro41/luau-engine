#include "engine/engine.h"

int main() {
    engine::Engine app;
    app.init();
    app.run();
    app.shutdown();
    return 0;
}
