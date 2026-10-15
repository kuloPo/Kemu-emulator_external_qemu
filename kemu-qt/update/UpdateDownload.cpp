#include "UpdateCore.h"
#include <windows.h>
#include <winhttp.h>
#include <array>
#include <fstream>
#include <set>
#include "unzip.h"
#include "iowin32.h"

namespace kemu::update {
struct Internet { HINTERNET h = nullptr; ~Internet() { if (h) WinHttpCloseHandle(h); } };
void download(const std::string& initial, const fs::path& output, uint64_t limit, const std::function<bool()>& cancelled, const std::function<void(uint64_t, uint64_t)>& progress) {
    auto url = fs::u8path(initial).wstring();
    Internet session{WinHttpOpen(L"Kemu-Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    require(session.h, "Cannot initialize HTTPS"); WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 30000);
    for (int redirect = 0; redirect < 8; ++redirect) {
        require(!cancelled(), "Update cancelled");
        URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts); parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
        require(WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts) && parts.nScheme == INTERNET_SCHEME_HTTPS && parts.nPort == 443, "Only HTTPS release downloads are allowed");
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        require(host == L"github.com" || host == L"raw.githubusercontent.com" || host == L"release-assets.githubusercontent.com" || host == L"objects.githubusercontent.com", "Unexpected release download host");
        Internet connection{WinHttpConnect(session.h, host.c_str(), parts.nPort, 0)}; require(connection.h, "HTTPS connection failed");
        Internet request{WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        require(request.h, "Cannot create HTTPS request"); DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        require(WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)), "Cannot set redirect policy");
        require(WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request.h, nullptr), "HTTPS request failed; check your connection");
        DWORD status = 0, length = sizeof(status);
        require(WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX), "Missing HTTP status");
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            length = 0; WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &length, WINHTTP_NO_HEADER_INDEX);
            require(length > 0 && length < 32768, "Invalid download redirect");
            std::wstring location(length / sizeof(wchar_t), L'\0');
            require(WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location.data(), &length, WINHTTP_NO_HEADER_INDEX), "Missing redirect");
            location.resize(wcslen(location.c_str())); url = location; continue;
        }
        require(status == 200, "Release download returned HTTP " + std::to_string(status));
        wchar_t contentLength[32]{}; length = sizeof(contentLength);
        uint64_t expected = 0;
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                contentLength, &length, WINHTTP_NO_HEADER_INDEX)) expected = _wcstoui64(contentLength, nullptr, 10);
        require(expected <= limit, "Download exceeds allowed size");
        if (progress) progress(0, expected);
        fs::create_directories(output.parent_path()); std::ofstream out(output, std::ios::binary | std::ios::trunc); require(bool(out), "Cannot create download file");
        std::vector<char> buffer(1024 * 1024); uint64_t total = 0;
        for (;;) {
            require(!cancelled(), "Update cancelled"); DWORD received = 0;
            require(WinHttpReadData(request.h, buffer.data(), DWORD(buffer.size()), &received), "Download interrupted");
            if (!received) break; total += received; require(total <= limit, "Download exceeds allowed size");
            out.write(buffer.data(), received); require(bool(out), "Download disk write failed");
            if (progress) progress(total, expected);
        }
        out.close(); require(bool(out) && total > 0, "Empty/incomplete download"); return;
    }
    throw std::runtime_error("Too many download redirects");
}
void extractPack(const fs::path& archive, const fs::path& output) {
    require(!fs::exists(output), "Extraction directory already exists"); fs::create_directories(output);
    zlib_filefunc64_def io{}; fill_win32_filefunc64W(&io);
    unzFile zip = unzOpen2_64(archive.c_str(), &io); require(zip != nullptr, "Invalid ZIP archive");
    struct Close { unzFile f; ~Close() { unzClose(f); } } close{zip};
    unz_global_info64 global{}; require(unzGetGlobalInfo64(zip, &global) == UNZ_OK && global.number_entry <= 100000, "Invalid ZIP entry count");
    std::set<std::string> seen; std::map<std::string, std::string> actual;
    uint64_t total = 0; int next = unzGoToFirstFile(zip);
    while (next == UNZ_OK) {
        unz_file_info64 info{}; require(unzGetCurrentFileInfo64(zip, &info, nullptr, 0, nullptr, 0, nullptr, 0) == UNZ_OK && info.size_filename < 1024, "Invalid ZIP entry");
        std::string name(info.size_filename + 1, '\0');
        require(unzGetCurrentFileInfo64(zip, &info, name.data(), ULONG(name.size()), nullptr, 0, nullptr, 0) == UNZ_OK, "Cannot read ZIP entry");
        name.resize(info.size_filename); require(name.find('\0') == name.npos && !name.empty(), "Invalid ZIP filename");
        bool directory = name.back() == '/'; if (directory) name.pop_back(); validatePath(name);
        require(name == "sha256sum.txt" || name == "Windows" || name == "Android" || name.rfind("Windows/",0) == 0 || name.rfind("Android/",0) == 0, "Unexpected ZIP root: " + name);
        auto canonical = name; for (auto& c : canonical) c = char(std::tolower(static_cast<unsigned char>(c)));
        require(seen.insert(canonical).second, "Duplicate ZIP entry");
        unsigned mode = (info.external_fa >> 16) & 0170000;
        require((mode == 0 || mode == 0100000 || (directory && mode == 0040000)) && !(info.flag & 1), "Links/special/encrypted ZIP entries are unsupported");
        safePath(output, name); auto destination = output / fs::u8path(name);
        if (directory) fs::create_directories(destination);
        else {
            require(name != "Windows" && name != "Android", "ZIP roots must be directories");
            total += info.uncompressed_size; require(info.uncompressed_size <= 8ull*1024*1024*1024 && total <= 16ull*1024*1024*1024, "ZIP payload too large");
            require(name != "sha256sum.txt" || info.uncompressed_size <= 8*1024*1024, "Oversized checksum file");
            fs::create_directories(destination.parent_path());
            require(unzOpenCurrentFile(zip) == UNZ_OK, "Cannot read compressed payload");
            std::ofstream out(destination, std::ios::binary); std::vector<char> buffer(256*1024); uint64_t bytes = 0; int got;
            while ((got = unzReadCurrentFile(zip, buffer.data(), unsigned(buffer.size()))) > 0) {
                bytes += got; require(bytes <= info.uncompressed_size, "ZIP size mismatch"); out.write(buffer.data(), got); require(bool(out), "ZIP disk write failed");
            }
            int crc = unzCloseCurrentFile(zip); out.close();
            require(got == 0 && crc == UNZ_OK && bytes == info.uncompressed_size && bool(out), "ZIP payload failed integrity verification");
            if (name != "sha256sum.txt") actual[name] = sha256(destination);
        }
        next = unzGoToNextFile(zip);
    }
    require(next == UNZ_END_OF_LIST_OF_FILE, "Truncated ZIP directory");
    require(actual == parseChecksums(readText(output / "sha256sum.txt")), "ZIP payload SHA-256 verification failed");
}
}

