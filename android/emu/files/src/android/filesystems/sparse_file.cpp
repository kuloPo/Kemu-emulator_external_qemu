#include "android/filesystems/sparse_file.h"

#include "aemu/base/files/ScopedStdioFile.h"
#include "android/utils/file_io.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#endif

namespace {
bool makeSparse(FILE* file) {
#ifdef _WIN32
    DWORD returned = 0;
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fileno(file)));
    return DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                           &returned, nullptr) != 0;
#else
    return true;
#endif
}

bool setLength(FILE* file, int64_t length) {
    if (fflush(file) != 0) {
        return false;
    }
#ifdef _WIN32
    return _chsize_s(fileno(file), length) == 0;
#else
    return ftruncate(fileno(file), length) == 0;
#endif
}
}  // namespace

bool android_createSparseFile(const char* path) {
    android::base::ScopedStdioFile file(android_fopen(path, "wb"));
    return file.get() && makeSparse(file.get());
}

bool android_copySparseFile(const char* source, const char* destination) {
    android::base::ScopedStdioFile input(android_fopen(source, "rb"));
    if (!input.get()) {
        return false;
    }
    int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef _WIN32
    flags |= O_BINARY;
#endif
    const int fd = android_open(destination, flags, 0600);
    if (fd < 0) {
        return false;
    }
    android::base::ScopedStdioFile output(fdopen(fd, "wb"));
    if (!output.get()) {
        close(fd);
        android_unlink(destination);
        return false;
    }
    bool success = makeSparse(output.get());
    std::array<unsigned char, 64 * 1024> buffer;
    int64_t length = 0;
    while (success) {
        const size_t count = fread(buffer.data(), 1, buffer.size(), input.get());
        if (count == 0) {
            success = !ferror(input.get());
            break;
        }
        length += count;
        if (std::all_of(buffer.begin(), buffer.begin() + count,
                        [](unsigned char byte) { return byte == 0; })) {
            success = fseeko(output.get(), count, SEEK_CUR) == 0;
        } else {
            success = fwrite(buffer.data(), 1, count, output.get()) == count;
        }
    }
    return success && setLength(output.get(), length);
}

bool android_replaceImageFile(const char* source, const char* destination) {
    const auto from = std::filesystem::u8path(source);
    const auto to = std::filesystem::u8path(destination);
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(from, to, error);
    return !error;
#endif
}
