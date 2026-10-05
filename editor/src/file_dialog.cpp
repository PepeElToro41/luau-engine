#include "file_dialog.hpp"

#include <SDL3/SDL_error.h>

bool NativeFileDialog::open_files(
	SDL_Window* window, 
	const SDL_DialogFileFilter* filters, 
	const int filter_count,
    const char* default_location, 
    const bool allow_many
) {
    if (this->pending) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->results.clear();
        this->failure.clear();
        this->finished = false;
    }
    this->pending = true;
    SDL_ShowOpenFileDialog(&NativeFileDialog::on_result, this, window, filters, filter_count, default_location, allow_many);
    return true;
}

void NativeFileDialog::on_result(void* userdata, const char* const* filelist, int /*filter*/) {
    NativeFileDialog* self = static_cast<NativeFileDialog*>(userdata);
    std::lock_guard<std::mutex> lock(self->mutex);
    if (filelist == nullptr) {
        // SDL_GetError is thread local, so it has to be read here.
        const char* reason = SDL_GetError();
        self->failure = (reason != nullptr && reason[0] != '\0') ? reason : "file dialog failed";
    } else {
        for (const char* const* it = filelist; *it != nullptr; ++it) {
            self->results.emplace_back(*it);
        }
    }
    self->finished = true;
}

bool NativeFileDialog::take(std::vector<std::filesystem::path>* out, std::string* error) {
    if (!this->pending) {
        return false;
    }
    std::lock_guard<std::mutex> lock(this->mutex);
    if (!this->finished) {
        return false;
    }
    for (const std::string& path : this->results) {
        out->emplace_back(path);
    }
    if (error != nullptr) {
        *error = this->failure;
    }
    this->results.clear();
    this->failure.clear();
    this->finished = false;
    this->pending = false;
    return true;
}
