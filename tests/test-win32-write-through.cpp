/* SPDX-License-Identifier: GPL-2.0-only */
#include <windows.h>
#include <winioctl.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <gtest/gtest.h>
#include "block/win32-write-through.h"

namespace {
constexpr DWORD kDurableFlags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;

class WriteThroughTest : public testing::Test {
protected:
    void SetUp() override {
        wchar_t directory[MAX_PATH], filename[MAX_PATH];
        ASSERT_NE(0u, GetTempPathW(MAX_PATH, directory));
        ASSERT_NE(0u, GetTempFileNameW(directory, L"kwt", 0, filename));
        path = filename;
        file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING,
                           kDurableFlags | FILE_FLAG_OVERLAPPED, nullptr);
        ASSERT_NE(INVALID_HANDLE_VALUE, file);
    }

    void TearDown() override {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (!path.empty()) EXPECT_TRUE(DeleteFileW(path.c_str()));
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
};

TEST_F(WriteThroughTest, RequiresBothCacheFlags) {
    EXPECT_FALSE(win32_write_through_is_durable(file, 0));
    EXPECT_FALSE(win32_write_through_is_durable(file, FILE_FLAG_NO_BUFFERING));
    EXPECT_FALSE(win32_write_through_is_durable(file, FILE_FLAG_WRITE_THROUGH));
}

TEST_F(WriteThroughTest, RejectsInvalidHandlesAndDirectories) {
    EXPECT_FALSE(win32_write_through_is_durable(INVALID_HANDLE_VALUE, kDurableFlags));
    wchar_t directory[MAX_PATH];
    ASSERT_NE(0u, GetTempPathW(MAX_PATH, directory));
    HANDLE dir = CreateFileW(directory, 0, FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ASSERT_NE(INVALID_HANDLE_VALUE, dir);
    EXPECT_FALSE(win32_write_through_is_durable(dir, kDurableFlags));
    CloseHandle(dir);
}

TEST_F(WriteThroughTest, SparseOverlappedWritesSurviveReopen) {
    wchar_t filesystem[32];
    ASSERT_TRUE(GetVolumeInformationByHandleW(file, nullptr, 0, nullptr, nullptr,
                                              nullptr, filesystem, 32));
    if (wcscmp(filesystem, L"NTFS")) GTEST_SKIP() << "NTFS is required";
    ASSERT_TRUE(win32_write_through_is_durable(file, kDurableFlags));
    DWORD transferred = 0;
    OVERLAPPED control{};
    BOOL sparse = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr,
                                  0, &transferred, &control);
    if (!sparse) {
        ASSERT_EQ(ERROR_IO_PENDING, GetLastError());
        ASSERT_TRUE(GetOverlappedResult(file, &control, &transferred, TRUE));
    }
    LARGE_INTEGER size;
    size.QuadPart = 32 * 1024 * 1024;
    ASSERT_TRUE(SetFilePointerEx(file, size, nullptr, FILE_BEGIN));
    ASSERT_TRUE(SetEndOfFile(file));
    ASSERT_TRUE(FlushFileBuffers(file));
    void* buffer = _aligned_malloc(8192, 4096);
    ASSERT_NE(nullptr, buffer);
    memset(buffer, 0xa7, 8192);
    for (DWORD offset : {0u, 16u * 1024 * 1024}) {
        OVERLAPPED operation{};
        operation.Offset = offset;
        BOOL written = WriteFile(file, buffer, 8192, nullptr, &operation);
        EXPECT_TRUE(written || GetLastError() == ERROR_IO_PENDING);
        EXPECT_TRUE(GetOverlappedResult(file, &operation, &transferred, TRUE));
        EXPECT_EQ(8192u, transferred);
    }
    _aligned_free(buffer);
    ASSERT_TRUE(CloseHandle(file));
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(INVALID_HANDLE_VALUE, file);
    unsigned char actual[8192];
    for (DWORD offset : {0u, 16u * 1024 * 1024}) {
        size.QuadPart = offset;
        ASSERT_TRUE(SetFilePointerEx(file, size, nullptr, FILE_BEGIN));
        ASSERT_TRUE(ReadFile(file, actual, sizeof(actual), &transferred, nullptr));
        ASSERT_EQ(sizeof(actual), transferred);
        for (auto byte : actual) ASSERT_EQ(0xa7, byte);
    }
}
}  // namespace
