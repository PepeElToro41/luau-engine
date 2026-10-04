// Win32 implementation of engine/platform/file.hpp. Every source under
// src/platform/ is compiled on every platform, so the whole file is guarded.
#if defined(_WIN32)

#include "engine/platform/file.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// HANDLE is a pointer and INVALID_HANDLE_VALUE is (HANDLE)-1, so storing it
// in File::handle as an i64 keeps -1 meaning closed on both platforms.
STATIC_ASSERT(sizeof(HANDLE) <= sizeof(i64), "a HANDLE must fit in File::handle");

namespace {

HANDLE to_handle(const File& file) {
    return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(file.handle));
}

// Room for the converted path, including the terminator. Far beyond MAX_PATH,
// which is as long as a path gets without the \\?\ prefix anyway.
constexpr int MAX_WIDE_PATH = 4096;

// UTF-8 -> UTF-16 for the W functions, so non-ASCII paths work regardless of
// the process code page. False when the path is not valid UTF-8 or too long.
bool to_wide(const char* path, wchar_t* out, const int capacity) {
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, capacity);
    return written > 0;
}

// ReadFile / WriteFile move at most a DWORD per call.
DWORD clamp_to_dword(const usz size) {
    return size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<DWORD>(size);
}

OVERLAPPED at_offset(const u64 offset) {
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return overlapped;
}

} // namespace

bool PLATFORM::file_open(File* file, const char* path, const FileAccess access) {
    PLATFORM::file_close(file);

    wchar_t wide[MAX_WIDE_PATH];
    if (!to_wide(path, wide, MAX_WIDE_PATH)) {
        return false;
    }

    DWORD desired_access = 0;
    DWORD share_mode = 0;
    DWORD creation = 0;
    switch (access) {
    case FILE_ACCESS_READ:
        desired_access = GENERIC_READ;
        share_mode = FILE_SHARE_READ;
        creation = OPEN_EXISTING;
        break;
    case FILE_ACCESS_WRITE:
        desired_access = GENERIC_WRITE;
        share_mode = 0;
        creation = CREATE_ALWAYS;
        break;
    }

    const HANDLE handle = CreateFileW(wide, desired_access, share_mode, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    file->handle = static_cast<i64>(reinterpret_cast<INT_PTR>(handle));
    return true;
}

void PLATFORM::file_close(File* file) {
    if (file->is_open()) {
        CloseHandle(to_handle(*file));
        file->handle = -1;
    }
}

bool PLATFORM::file_size(const File& file, u64* out_size) {
    if (!file.is_open()) {
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(to_handle(file), &size) || size.QuadPart < 0) {
        return false;
    }
    *out_size = static_cast<u64>(size.QuadPart);
    return true;
}

bool PLATFORM::file_read(const File& file, const u64 offset, void* out, const usz size) {
    if (!file.is_open()) {
        return false;
    }
    // A synchronous handle with an OVERLAPPED gives a positional read. The
    // call may still return fewer bytes than asked, so loop until everything
    // arrived or the file ended.
    u8* cursor = static_cast<u8*>(out);
    usz remaining = size;
    u64 position = offset;
    while (remaining > 0) {
        OVERLAPPED overlapped = at_offset(position);
        DWORD got = 0;
        if (!ReadFile(to_handle(file), cursor, clamp_to_dword(remaining), &got, &overlapped)) {
            return false; // includes ERROR_HANDLE_EOF for a start past the end
        }
        if (got == 0) {
            return false; // end of file before `size` bytes
        }
        cursor += got;
        remaining -= got;
        position += got;
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
        OVERLAPPED overlapped = at_offset(position);
        DWORD put = 0;
        if (!WriteFile(to_handle(file), cursor, clamp_to_dword(remaining), &put, &overlapped)) {
            return false;
        }
        if (put == 0) {
            return false; // no progress: treat as an error rather than spin
        }
        cursor += put;
        remaining -= put;
        position += put;
    }
    return true;
}

#endif // _WIN32
