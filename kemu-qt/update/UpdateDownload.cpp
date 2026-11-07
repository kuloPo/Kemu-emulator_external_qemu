#include "UpdateCore.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include "7zCrc.h"
#include "Alloc.h"
#include "Xz.h"
#include "XzCrc64.h"

namespace kemu::update {
namespace {
// Streams the decompressed bytes of an xz file; block CRCs are checked by the decoder.
class XzReader {
public:
    explicit XzReader(const fs::path& path) : mIn(path, std::ios::binary), mInput(1024*1024) {
        require(bool(mIn), "Cannot open update pack");
        static std::once_flag tables; std::call_once(tables, [] { CrcGenerateTable(); Crc64GenerateTable(); });
        XzUnpacker_Construct(&mState, &g_Alloc); XzUnpacker_Init(&mState);
    }
    ~XzReader() { XzUnpacker_Free(&mState); }
    XzReader(const XzReader&) = delete; XzReader& operator=(const XzReader&) = delete;
    // Returns fewer bytes than requested only at the end of the stream.
    size_t read(char* data, size_t size) {
        size_t done = 0;
        while (done < size && !mFinished) {
            if (mPosition == mAvailable && !mEof) {
                mIn.read(reinterpret_cast<char*>(mInput.data()), std::streamsize(mInput.size())); require(!mIn.bad(), "Cannot read update pack");
                mAvailable = size_t(mIn.gcount()); mPosition = 0; mEof = mIn.eof(); continue;
            }
            SizeT produced = size - done, consumed = mAvailable - mPosition; ECoderStatus status;
            SRes result = XzUnpacker_Code(&mState, reinterpret_cast<Byte*>(data + done), &produced, mInput.data() + mPosition, &consumed, mEof, CODER_FINISH_ANY, &status);
            require(result == SZ_OK, "Update pack failed xz verification (error " + std::to_string(result) + ")");
            mPosition += consumed; done += produced;
            if (!produced && !consumed) {
                require(mEof && mPosition == mAvailable, "xz decoder stalled");
                require(XzUnpacker_IsStreamWasFinished(&mState), "Truncated xz update pack"); mFinished = true;
            }
        }
        return done;
    }
private:
    std::ifstream mIn; std::vector<Byte> mInput; size_t mPosition = 0, mAvailable = 0; bool mEof = false, mFinished = false;
    CXzUnpacker mState;
};
uint64_t tarNumber(const char* field, size_t size) {
    uint64_t value = 0; size_t i = 0;
    for (; i < size && field[i] >= '0' && field[i] <= '7'; ++i) { require(value >> 60 == 0, "Invalid tar number"); value = value * 8 + unsigned(field[i] - '0'); }
    require(i > 0, "Invalid tar number");
    for (; i < size; ++i) require(field[i] == '\0' || field[i] == ' ', "Invalid tar number");
    return value;
}
std::string tarText(const char* field, size_t size) { return std::string(field, std::find(field, field + size, '\0')); }
bool zeros(const char* data, size_t size) { return std::all_of(data, data + size, [](char c) { return c == 0; }); }
}
void extractPack(const fs::path& archive, const fs::path& output) {
    require(!fs::exists(output), "Extraction directory already exists"); fs::create_directories(output);
    XzReader xz(archive); std::array<char, 512> header{}; std::vector<char> buffer(256*1024);
    std::set<std::string> seen; std::map<std::string, std::string> actual; uint64_t total = 0;
    for (size_t entries = 0;; ++entries) {
        require(entries <= 100000, "Invalid tar entry count");
        require(xz.read(header.data(), header.size()) == header.size(), "Truncated tar archive");
        if (zeros(header.data(), header.size())) break;
        unsigned sum = 0; for (size_t i = 0; i < header.size(); ++i) sum += i >= 148 && i < 156 ? ' ' : static_cast<unsigned char>(header[i]);
        require(tarNumber(&header[148], 8) == sum, "Tar header checksum mismatch");
        require(std::memcmp(&header[257], "ustar\0" "00", 8) == 0, "Only POSIX ustar entries are supported");
        char type = header[156]; bool directory = type == '5';
        require((type == '0' || type == '\0' || directory) && tarText(&header[157], 100).empty(), "Links/special tar entries are unsupported");
        auto name = tarText(&header[0], 100), prefix = tarText(&header[345], 155);
        if (!prefix.empty()) name = prefix + "/" + name;
        if (directory && !name.empty() && name.back() == '/') name.pop_back();
        validatePath(name);
        require(name == "sha256sum.txt" || name == "Windows" || name == "Android" || name.rfind("Windows/",0) == 0 || name.rfind("Android/",0) == 0, "Unexpected pack root: " + name);
        auto canonical = name; for (auto& c : canonical) c = char(std::tolower(static_cast<unsigned char>(c)));
        require(seen.insert(canonical).second, "Duplicate pack entry");
        uint64_t size = tarNumber(&header[124], 12); require(!directory || size == 0, "Tar directory has data");
        safePath(output, name); auto destination = output / fs::u8path(name);
        if (directory) { fs::create_directories(destination); continue; }
        require(name != "Windows" && name != "Android", "Pack roots must be directories");
        total += size; require(size <= 8ull*1024*1024*1024 && total <= 16ull*1024*1024*1024, "Pack payload too large");
        require(name != "sha256sum.txt" || size <= 8*1024*1024, "Oversized checksum file");
        fs::create_directories(destination.parent_path());
        std::ofstream out(destination, std::ios::binary);
        for (uint64_t left = size; left;) {
            auto chunk = size_t(std::min<uint64_t>(left, buffer.size()));
            require(xz.read(buffer.data(), chunk) == chunk, "Truncated tar payload");
            out.write(buffer.data(), std::streamsize(chunk)); require(bool(out), "Tar disk write failed"); left -= chunk;
        }
        out.close(); require(bool(out), "Tar disk write failed");
        auto padding = size_t((512 - size % 512) % 512);
        require(xz.read(header.data(), padding) == padding && zeros(header.data(), padding), "Invalid tar padding");
        if (name != "sha256sum.txt") actual[name] = sha256(destination);
    }
    // The second end-of-archive block and the record padding must run to the end of the stream.
    size_t trailing = 0, got;
    while ((got = xz.read(buffer.data(), buffer.size())) > 0) {
        trailing += got; require(trailing <= 1024*1024 && zeros(buffer.data(), got), "Unexpected data after tar archive");
    }
    require(trailing >= 512, "Truncated tar archive");
    require(actual == parseChecksums(readText(output / "sha256sum.txt")), "Pack payload SHA-256 verification failed");
}
}

