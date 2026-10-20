#include "android/filesystems/sparse_file.h"

#include "android/base/testing/TestTempDir.h"
#include "android/utils/file_io.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
std::string readImage(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}  // namespace

TEST(SparseFile, PreservesBytesAndTrailingHoles) {
    android::base::TestTempDir temporary("sparse-image");
    const auto source = temporary.makeSubPath("source.img");
    const auto destination = temporary.makeSubPath("data.img");
    std::string bytes(4 * 1024 * 1024 + 17, '\0');
    bytes[0] = 'a';
    bytes[65535] = 'b';
    bytes[65536] = 'c';
    bytes[2 * 1024 * 1024 + 3] = 'd';
    std::ofstream(std::filesystem::u8path(source), std::ios::binary)
            .write(bytes.data(), bytes.size());
    ASSERT_TRUE(android_copySparseFile(source.c_str(), destination.c_str()));
    EXPECT_EQ(bytes, readImage(std::filesystem::u8path(destination)));
    EXPECT_EQ(bytes.size(), std::filesystem::file_size(std::filesystem::u8path(destination)));
#ifdef _WIN32
    const auto path = std::filesystem::u8path(destination);
    EXPECT_NE(0, GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_SPARSE_FILE);
    DWORD high = 0;
    const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
    EXPECT_LT((uint64_t(high) << 32) | low, bytes.size() / 2);
#endif
    EXPECT_FALSE(android_copySparseFile(source.c_str(), destination.c_str()));
    EXPECT_EQ(bytes, readImage(std::filesystem::u8path(destination)));
}

TEST(SparseFile, MissingSourceDoesNotCreateDestination) {
    android::base::TestTempDir temporary("sparse-image");
    const auto destination = temporary.makeSubPath("data.img");
    EXPECT_FALSE(android_copySparseFile(temporary.makeSubPath("missing").c_str(),
                                       destination.c_str()));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(destination)));
}

TEST(SparseFile, PublishesCompletedImage) {
    android::base::TestTempDir temporary("sparse-image");
    const auto destination = temporary.makeSubPath("data.img");
    const auto staging = temporary.makeSubPath("data.img.raw-init");
    std::ofstream(std::filesystem::u8path(destination)) << "old";
    ASSERT_TRUE(android_createSparseFile(staging.c_str()));
    std::ofstream(std::filesystem::u8path(staging)) << "new";
    ASSERT_TRUE(android_replaceImageFile(staging.c_str(), destination.c_str()));
    EXPECT_EQ("new", readImage(std::filesystem::u8path(destination)));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(staging)));
    EXPECT_FALSE(android_replaceImageFile(staging.c_str(), destination.c_str()));
    EXPECT_EQ("new", readImage(std::filesystem::u8path(destination)));
}
