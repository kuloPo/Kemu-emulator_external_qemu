#pragma once
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace kemu::storage {
namespace fs = std::filesystem;
inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit Handle(HANDLE handle) : value(handle) {}
};
inline fs::path locationPath(const fs::path& avd) { return avd / "storage-location.txt"; }
inline fs::path moveRequestPath(const fs::path& avd) { return avd / "storage-move.txt"; }
inline fs::path readPath(const fs::path& record) {
    std::ifstream file(record, std::ios::binary);
    std::string version, value, extra;
    require(bool(std::getline(file, version)) && version == "1" &&
            bool(std::getline(file, value)) && !std::getline(file, extra) && file.eof(),
            "Cannot read the disk location record.");
    const auto path = fs::u8path(value);
    require(path.is_absolute() && path.filename() == "userdata-qemu.img" &&
            value.find_first_of("\r\n\0", 0, 3) == std::string::npos,
            "Invalid disk location.");
    return path;
}
inline fs::path imagePath(const fs::path& avd) {
    return fs::exists(locationPath(avd)) ? readPath(locationPath(avd)) : avd / "kemu.avd/userdata-qemu.img";
}
inline void writePath(const fs::path& record, const fs::path& path, bool replace) {
    const auto data = "1\n" + path.u8string() + "\n";
    const auto temporary = fs::path(record.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    try {
        {
            Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr)};
            DWORD written = 0;
            require(file.value != INVALID_HANDLE_VALUE &&
                    WriteFile(file.value, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size() &&
                    FlushFileBuffers(file.value), "Cannot save the disk location.");
        }
        require(MoveFileExW(temporary.c_str(), record.c_str(), MOVEFILE_WRITE_THROUGH |
                            (replace ? MOVEFILE_REPLACE_EXISTING : 0)), "Cannot publish the disk location.");
    } catch (...) { DeleteFileW(temporary.c_str()); throw; }
}
inline fs::path moveDestination(const fs::path& avd, const fs::path& directory) {
    require(!fs::exists(avd / "storage-growth.txt"), "Finish disk expansion before moving the disk.");
    require(fs::is_directory(directory), "The destination directory is unavailable.");
    const auto destination = fs::canonical(directory) / "userdata-qemu.img";
    require(!fs::exists(destination), "The destination already contains a data image. Choose another directory.");
    require(!fs::exists(fs::path(destination.wstring() + L".qcow2")), "The destination contains a QCOW2 image.");
    const auto image = imagePath(avd);
    require(fs::is_regular_file(image), "The current data image is unavailable.");
    require(!fs::exists(fs::path(image.wstring() + L".qcow2")), "Moving requires a single RAW data image.");
    DWORD high = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD low = GetCompressedFileSizeW(image.c_str(), &high);
    require(low != INVALID_FILE_SIZE || GetLastError() == ERROR_SUCCESS, "Cannot read the disk's allocated size.");
    require(fs::space(directory).available >= ((uint64_t{high} << 32) | low), "Not enough free space at the destination.");
    wchar_t volume[MAX_PATH]{};
    DWORD flags = 0;
    require(GetVolumePathNameW(destination.c_str(), volume, MAX_PATH) &&
            GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) &&
            (flags & FILE_SUPPORTS_SPARSE_FILES), "The destination must support sparse files (for example, NTFS).");
    return destination;
}
inline void requestMove(const fs::path& avd, const fs::path& directory) {
    require(!fs::exists(moveRequestPath(avd)), "A disk move is already pending.");
    writePath(moveRequestPath(avd), moveDestination(avd, directory), false);
}
inline std::vector<FILE_ALLOCATED_RANGE_BUFFER> allocatedRanges(HANDLE file, uint64_t size) {
    std::vector<FILE_ALLOCATED_RANGE_BUFFER> ranges;
    FILE_ALLOCATED_RANGE_BUFFER query{};
    while (uint64_t(query.FileOffset.QuadPart) < size) {
        query.Length.QuadPart = size - query.FileOffset.QuadPart;
        FILE_ALLOCATED_RANGE_BUFFER batch[256]{};
        DWORD bytes = 0;
        const BOOL ok = DeviceIoControl(file, FSCTL_QUERY_ALLOCATED_RANGES, &query, sizeof(query),
                                        batch, sizeof(batch), &bytes, nullptr);
        require(ok || GetLastError() == ERROR_MORE_DATA, "Cannot read the data image's sparse ranges.");
        const auto count = bytes / sizeof(batch[0]);
        ranges.insert(ranges.end(), batch, batch + count);
        if (ok) break;
        require(count > 0, "Cannot advance through the data image's sparse ranges.");
        const auto next = batch[count - 1].FileOffset.QuadPart + batch[count - 1].Length.QuadPart;
        require(next > query.FileOffset.QuadPart, "Invalid sparse range.");
        query.FileOffset.QuadPart = next;
    }
    return ranges;
}
inline void seek(HANDLE file, uint64_t position) {
    LARGE_INTEGER offset{}; offset.QuadPart = position;
    require(SetFilePointerEx(file, offset, nullptr, FILE_BEGIN), "Cannot seek in the data image.");
}
// The launcher holds the instance lease and waits for the VM to exit first.
// A failed/interrupted copy never becomes the active disk. An interrupted move
// may leave a staging file or a verified duplicate, but keeps the original data.
inline bool applyMove(const fs::path& avd, const std::function<void(int)>& progress = {}) {
    if (!fs::exists(moveRequestPath(avd))) return false;
    const auto destination = readPath(moveRequestPath(avd));
    require(fs::remove(moveRequestPath(avd)), "Cannot consume the disk move request.");
    moveDestination(avd, destination.parent_path());
    const auto original = imagePath(avd);
    Handle source{CreateFileW(original.c_str(), GENERIC_READ | DELETE, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(source.value != INVALID_HANDLE_VALUE, "Cannot open the data image exclusively. Close Kemu and try again.");
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(source.value, &info) &&
            !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)),
            "The data image must be a regular file.");
    LARGE_INTEGER size{};
    require(GetFileSizeEx(source.value, &size) && size.QuadPart >= 4096, "Invalid data image size.");
    unsigned char magic[4]{}; DWORD count = 0;
    require(ReadFile(source.value, magic, sizeof(magic), &count, nullptr) && count == sizeof(magic), "Cannot read the data image.");
    require(!(magic[0] == 'Q' && magic[1] == 'F' && magic[2] == 'I' && magic[3] == 0xfb), "Moving requires a RAW data image.");
    const auto ranges = allocatedRanges(source.value, size.QuadPart);
    uint64_t total = 0;
    for (const auto& range : ranges) total += range.Length.QuadPart;
    const auto temporary = fs::path(destination.wstring() + L".kemu-moving-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Handle target{CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(target.value != INVALID_HANDLE_VALUE, "Cannot create a disk at the destination.");
    bool committed = false;
    try {
        DWORD returned = 0;
        require(DeviceIoControl(target.value, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr),
                "Cannot create a sparse disk at the destination.");
        seek(target.value, size.QuadPart);
        require(SetEndOfFile(target.value), "Cannot set the destination disk size.");
        std::vector<char> buffer(4 * 1024 * 1024), comparison(buffer.size());
        uint64_t done = 0;
        int reported = -1;
        for (bool verify : {false, true}) {
            for (const auto& range : ranges) {
                seek(source.value, range.FileOffset.QuadPart); seek(target.value, range.FileOffset.QuadPart);
                for (uint64_t left = range.Length.QuadPart; left;) {
                    const DWORD block = DWORD((std::min)(left, uint64_t(buffer.size())));
                    require(ReadFile(source.value, buffer.data(), block, &count, nullptr) && count == block,
                            "Cannot read the original disk.");
                    if (verify) {
                        require(ReadFile(target.value, comparison.data(), block, &count, nullptr) && count == block &&
                                std::equal(buffer.begin(), buffer.begin() + block, comparison.begin()), "Disk verification failed.");
                    } else {
                        require(WriteFile(target.value, buffer.data(), block, &count, nullptr) && count == block,
                                "Cannot copy the disk. Check free space and destination permissions.");
                    }
                    left -= block; done += block;
                    const int percent = int(done * 100 / (total * 2));
                    if (progress && percent != reported) { reported = percent; progress(percent); }
                }
            }
            if (!verify) require(FlushFileBuffers(target.value), "Cannot flush the copied disk.");
        }
        const auto name = destination.wstring();
        std::vector<unsigned char> renameBuffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(renameBuffer.data());
        rename->ReplaceIfExists = FALSE;
        rename->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
        std::copy(name.begin(), name.end(), rename->FileName);
        require(SetFileInformationByHandle(target.value, FileRenameInfo, rename, DWORD(renameBuffer.size())),
                "Cannot finalize the copied disk. The destination may already exist.");
        require(FlushFileBuffers(target.value), "Cannot flush the finalized disk.");
        writePath(locationPath(avd), destination, true);
        committed = true;
        FILE_DISPOSITION_INFO remove{TRUE};
        require(SetFileInformationByHandle(source.value, FileDispositionInfo, &remove, sizeof(remove)),
                "The disk was moved, but the original copy could not be removed.");
    } catch (...) {
        if (!committed) {
            FILE_DISPOSITION_INFO remove{TRUE};
            SetFileInformationByHandle(target.value, FileDispositionInfo, &remove, sizeof(remove));
        }
        throw;
    }
    return true;
}
}
