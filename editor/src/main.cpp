#include "app.hpp"

int main() {
    App app;
    const bool ok = app.init();
    if (ok) {
        app.run();
    }
    app.shutdown();
    return ok ? 0 : 1;
}
