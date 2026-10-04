// POSIX implementation of engine/platform/file.hpp. Every source under
// src/platform/ is compiled on every platform, so the whole file is guarded.
#if defined(__linux__)

#include "engine/platform/file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// pread/pwrite take an off_t; the public API promises 64-bit offsets.
STATIC_ASSERT(sizeof(off_t) == 8, "off_t must be 64 bits; build with _FILE_OFFSET_BITS=64");

namespace {

int to_fd(const File& file) {
    return static_cast<int>(file.handle);
}

} // namespace

bool PLATFORM::file_open(File* file, const char* path, const FileAccess access) {
    PLATFORM::file_close(file);

    int flags = O_CLOEXEC;
    switch (access) {
    case FILE_ACCESS_READ: flags |= O_RDONLY; break;
    case FILE_ACCESS_WRITE: flags |= O_WRONLY | O_CREAT | O_TRUNC; break;
    }

    int fd = -1;
    do {
        fd = ::open(path, flags, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return false;
    }
    file->handle = fd;
    return true;
}

void PLATFORM::file_close(File* file) {
    if (file->is_open()) {
        ::close(to_fd(*file));
        file->handle = -1;
    }
}

bool PLATFORM::file_size(const File& file, u64* out_size) {
    if (!file.is_open()) {
        return false;
    }
    struct stat info;
    if (::fstat(to_fd(file), &info) != 0 || info.st_size < 0) {
        return false;
    }
    *out_size = static_cast<u64>(info.st_size);
    return true;
}

bool PLATFORM::file_read(const File& file, const u64 offset, void* out, const usz size) {
    if (!file.is_open()) {
        return false;
    }
    // pread may return fewer bytes than asked (signals, pipes, large sizes),
    // so loop until everything arrived or the file ended.
    u8* cursor = static_cast<u8*>(out);
    usz remaining = size;
    u64 position = offset;
    while (remaining > 0) {
        const ssize_t got = ::pread(to_fd(file), cursor, remaining, static_cast<off_t>(position));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (got == 0) {
            return false; // end of file before `size` bytes
        }
        cursor += got;
        remaining -= static_cast<usz>(got);
        position += static_cast<u64>(got);
    }
    return true;
}

bool PLATFORM::file_write(const File& file, const u64 offset, const void* data, const usz size) {
    if (!file.is_open()) {
        return false;
    }
    const u8* cursor = static_cast<const u8*>(data);
    usz remaining = size;
    u64 position = offset;
    while (remaining > 0) {
        const ssize_t put = ::pwrite(to_fd(file), cursor, remaining, static_cast<off_t>(position));
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (put == 0) {
            return false; // no progress: treat as an error rather than spin
        }
        cursor += put;
        remaining -= static_cast<usz>(put);
        position += static_cast<u64>(put);
    }
    return true;
}

#endif // __linux__
