#include "engine/engine.h"

int main() {
    Engine app;
    app.init();
    app.run();
    app.shutdown();
    return 0;
}
