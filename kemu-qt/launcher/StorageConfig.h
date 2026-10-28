#pragma once
#include "StorageLocation.h"
#include <windows.h>
#include <winioctl.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace kemu::storage {
namespace fs = std::filesystem;
constexpr uint64_t GiB = uint64_t{1024} * 1024 * 1024;
constexpr uint64_t MaxGiB = 16383;

inline fs::path requestPath(const fs::path& avd) { return avd / "storage-growth.txt"; }
struct Usage { uint64_t capacity, allocated; };
struct Request { uint64_t original = 0, target = 0; };

inline Usage usage(const fs::path& avd) {
    const auto image = imagePath(avd);
    const auto length = fs::file_size(image);
    DWORD high = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD low = GetCompressedFileSizeW(image.c_str(), &high);
    require(low != INVALID_FILE_SIZE || GetLastError() == ERROR_SUCCESS,
            "Cannot read the data image's allocated size.");
    return {length, (uint64_t{high} << 32) | low};
}

inline Request readRequest(const fs::path& avd) {
    const auto path = requestPath(avd);
    if (!fs::exists(path)) return {};
    std::ifstream file(path);
    Request request;
    std::string version, extra;
    require(bool(file >> version >> request.original >> request.target) && version == "1",
            "Invalid storage expansion request.");
    require(!(file >> extra) && file.eof() && request.original > 0 &&
            request.original % 4096 == 0 && request.target > request.original &&
            request.target % GiB == 0 && request.target / GiB <= MaxGiB,
            "Invalid storage expansion size.");
    return request;
}

inline void requestGrowth(const fs::path& avd, uint64_t targetGiB) {
    require(!fs::exists(moveRequestPath(avd)), "A disk move is pending.");
    require(!fs::exists(requestPath(avd)), "A storage expansion is already pending.");
    const auto current = usage(avd);
    require(targetGiB > 0 && targetGiB <= MaxGiB &&
            targetGiB * GiB > current.capacity && current.capacity % 4096 == 0,
            "The new capacity must be larger than the current disk.");
    const auto temporary = avd / ("storage-growth-" + std::to_string(GetCurrentProcessId()) + ".tmp");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << "1 " << current.capacity << ' ' << targetGiB * GiB << '\n';
        file.close();
        require(bool(file), "Cannot save the storage expansion request.");
    }
    if (!MoveFileExW(temporary.c_str(), requestPath(avd).c_str(), MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temporary);
        throw std::runtime_error("Cannot publish the storage expansion request.");
    }
}

// The launcher calls this after the previous emulator has exited and while
// holding the instance lease. The guest completes the filesystem resize.
inline uint64_t applyGrowth(const fs::path& avd) {
    const auto request = readRequest(avd);
    if (!request.target) return 0;
    const auto image = imagePath(avd);
    require(!fs::exists(fs::path(image.wstring() + L".qcow2")),
            "Storage expansion requires a single RAW data image.");
    Handle file{CreateFileW(image.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(file.value != INVALID_HANDLE_VALUE, "Cannot open the data image exclusively.");
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(file.value, &info) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "The data image must be a regular file.");
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file.value, &size) &&
            (uint64_t(size.QuadPart) == request.original || uint64_t(size.QuadPart) == request.target),
            "The data image size changed after expansion was requested.");
    unsigned char magic[4]{};
    DWORD count = 0;
    require(ReadFile(file.value, magic, sizeof(magic), &count, nullptr) && count == sizeof(magic),
            "Cannot read the data image.");
    require(!(magic[0] == 'Q' && magic[1] == 'F' && magic[2] == 'I' && magic[3] == 0xfb),
            "QCOW2 data images cannot be expanded here.");
    if (uint64_t(size.QuadPart) < request.target) {
        DWORD returned = 0;
        require(DeviceIoControl(file.value, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                                &returned, nullptr) != 0,
                "Cannot enable sparse allocation for the data image.");
        LARGE_INTEGER end{};
        end.QuadPart = request.target;
        require(SetFilePointerEx(file.value, end, nullptr, FILE_BEGIN) && SetEndOfFile(file.value),
                "Cannot expand the data image.");
        require(FlushFileBuffers(file.value) != 0, "Cannot flush the expanded data image.");
    }
    return request.target / GiB;
}

inline void completeGrowth(const fs::path& avd, const Request& expected) {
    const auto current = readRequest(avd);
    require(current.original == expected.original && current.target == expected.target,
            "The storage expansion request changed.");
    require(fs::remove(requestPath(avd)), "Cannot finish the storage expansion request.");
}
}
