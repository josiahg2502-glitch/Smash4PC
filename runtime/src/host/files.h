#pragma once

// The POSIX file calls the VFS uses. On Windows: wide paths, binary mode,
// 64-bit sizes and positional I/O through OVERLAPPED offsets.

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cafe::host {

#ifdef _WIN32
using FileStat = struct _stat64;

inline int file_stat(const std::filesystem::path& path, FileStat& st) { return _wstat64(path.c_str(), &st); }
inline int file_fstat(int fd, FileStat& st) { return _fstat64(fd, &st); }
inline int file_open(const std::filesystem::path& path, int flags, int mode = 0644) {
    return _wopen(path.c_str(), flags | O_BINARY | O_NOINHERIT, mode);
}
inline int file_close(int fd) { return _close(fd); }
inline int file_sync(int fd) { return _commit(fd); }

inline int64_t file_transfer(int fd, void* buffer, uint64_t size, uint64_t offset, bool write) {
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    const DWORD wanted = static_cast<DWORD>(size < (1u << 30) ? size : (1u << 30));
    DWORD done = 0;
    const BOOL ok = write ? WriteFile(handle, buffer, wanted, &done, &at)
                          : ReadFile(handle, buffer, wanted, &done, &at);
    if (ok) return done;
    if (GetLastError() == ERROR_HANDLE_EOF) return 0;
    errno = EIO;
    return -1;
}
inline int64_t file_pread(int fd, void* buffer, uint64_t size, uint64_t offset) {
    return file_transfer(fd, buffer, size, offset, false);
}
inline int64_t file_pwrite(int fd, const void* buffer, uint64_t size, uint64_t offset) {
    return file_transfer(fd, const_cast<void*>(buffer), size, offset, true);
}
#else
using FileStat = struct stat;

inline int file_stat(const std::filesystem::path& path, FileStat& st) { return ::stat(path.c_str(), &st); }
inline int file_fstat(int fd, FileStat& st) { return ::fstat(fd, &st); }
inline int file_open(const std::filesystem::path& path, int flags, int mode = 0644) {
    return ::open(path.c_str(), flags | O_CLOEXEC, mode);
}
inline int file_close(int fd) { return ::close(fd); }
inline int file_sync(int fd) { return ::fsync(fd); }
inline int64_t file_pread(int fd, void* buffer, uint64_t size, uint64_t offset) {
    return ::pread(fd, buffer, size, static_cast<off_t>(offset));
}
inline int64_t file_pwrite(int fd, const void* buffer, uint64_t size, uint64_t offset) {
    return ::pwrite(fd, buffer, size, static_cast<off_t>(offset));
}
#endif

} // namespace cafe::host
