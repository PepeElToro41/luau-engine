#pragma once

#include "engine/defines.hpp"

// Platform layer: files. Thin, unbuffered wrappers over the OS file API
// (Win32 on Windows, POSIX on Linux) so the rest of core never touches
// <cstdio>, <windows.h> or <unistd.h>, and so offsets are 64-bit everywhere.
// Each platform implements this header in engine/core/src/platform/<os>/.
//
//     File file;
//     if (PLATFORM::file_open(&file, "rock.lunaasset", FILE_ACCESS_READ)) {
//         u64 size = 0;
//         PLATFORM::file_size(file, &size);
//         PLATFORM::file_read(file, offset, buffer, bytes);
//         PLATFORM::file_close(&file);
//     }
//
// Reads and writes are positional: every call says where in the file it
// operates, nothing tracks a cursor, so a reader can jump between chunks
// without seeking first. All paths are UTF-8; Windows converts them to the
// wide API. Failures return false and print nothing: the caller knows what
// it was doing and reports in its own words.

#if !defined(_WIN32) && !defined(__linux__)
#error "engine/platform/file.hpp: no file implementation for this platform (see engine/core/src/platform/)"
#endif

enum FileAccess : u32 {
    // The file must exist; it is opened read-only and others may read it too.
    FILE_ACCESS_READ,
    // The file is created, or truncated to zero if it exists, and opened for
    // writing only.
    FILE_ACCESS_WRITE,
};

// An open file. Plain data: copying it copies the handle, not the file, so
// close exactly once. A default-constructed File is closed.
struct File {
    // The OS handle (a HANDLE on Windows, a file descriptor on Linux), or
    // -1 when closed. Only the platform layer reads it.
    i64 handle = -1;

    bool is_open() const { return this->handle != -1; }
};

namespace PLATFORM {

// Opens `path` as `access` describes into `file`, closing whatever it held
// before. False when the file cannot be opened; `file` is then closed.
bool file_open(File* file, const char* path, FileAccess access);

// Closes `file` if it is open. Closing a closed file does nothing.
void file_close(File* file);

// The current size of the open file in bytes. False on a closed file or
// when the OS cannot say.
bool file_size(const File& file, u64* out_size);

// Reads exactly `size` bytes starting at byte `offset` into `out`. False on
// a closed file, a read error or when the file ends first; `out` is then
// unspecified. A `size` of 0 reads nothing and succeeds.
bool file_read(const File& file, u64 offset, void* out, usz size);

// Writes exactly `size` bytes from `data` at byte `offset`, growing the file
// as needed. False on a closed file or a write error, after which the file's
// contents past `offset` are unspecified. A `size` of 0 writes nothing and
// succeeds.
bool file_write(const File& file, u64 offset, const void* data, usz size);

} // namespace PLATFORM
