#pragma once

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_video.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// The OS file picker, through SDL3. SDL shows the dialog asynchronously and
// may deliver the result on another thread, so the callback only stores the
// chosen paths under a lock; the main loop collects them once per frame with
// take(). One dialog at a time: open_files() is ignored while one is up.
//
//     if (ImGui::MenuItem("Import...") && !dialog.is_pending()) {
//         dialog.open_files(window, FILTERS, 3, project.root.c_str(), true);
//     }
//     ...each frame...
//     std::vector<std::filesystem::path> picked;
//     std::string error;
//     if (dialog.take(&picked, &error)) { ... }
//
// On Linux the dialog goes through the XDG portal or GTK, which needs the
// SDL event loop pumped; the editor polls events every frame so that holds.
// The object must outlive the dialog: it is written to from the callback.
struct NativeFileDialog {
    // Shows an open-file dialog modal for `window`. `filters` must stay valid
    // until the result arrives (pass a static array). `default_location` may
    // be null. Returns false if a dialog is already pending.
    bool open_files(SDL_Window* window, const SDL_DialogFileFilter* filters, int filter_count,
                    const char* default_location, bool allow_many);

    // Moves the files picked since the last call into `out` and returns true
    // once per finished dialog, also for a cancel (empty `out`). `error` is
    // set when the dialog itself failed. Call from the main thread.
    bool take(std::vector<std::filesystem::path>* out, std::string* error);

    // A dialog is up and has not reported back yet.
    bool is_pending() const { return this->pending; }

private:
    static void on_result(void* userdata, const char* const* filelist, int filter);

    std::mutex mutex;
    std::vector<std::string> results; // guarded by mutex
    std::string failure;              // guarded by mutex
    bool finished = false;            // guarded by mutex
    bool pending = false;             // main thread only
};
