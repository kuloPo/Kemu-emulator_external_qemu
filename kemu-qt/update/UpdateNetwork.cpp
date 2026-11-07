#include "UpdateCore.h"
#include <windows.h>
#include <winhttp.h>
#include <fstream>

namespace kemu::update {
struct Internet { HINTERNET h = nullptr; ~Internet() { if (h) WinHttpCloseHandle(h); } };
static void requireNetwork(bool ok, const std::string& message) { if (!ok) throw NetworkError(message); }
static void transfer(const std::string& initial, const std::function<void(const char*, size_t)>& write, uint64_t limit, const std::function<bool()>& cancelled, const std::function<void(uint64_t, uint64_t)>& progress) {
    auto url = fs::u8path(initial).wstring();
    Internet session{WinHttpOpen(L"Kemu-Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    require(session.h, "Cannot initialize HTTPS"); WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 30000);
    for (int redirect = 0; redirect < 8; ++redirect) {
        require(!cancelled(), "Update cancelled");
        URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts); parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
        require(WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts) && parts.nScheme == INTERNET_SCHEME_HTTPS && parts.nPort == 443, "Only HTTPS release downloads are allowed");
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        require(host == L"github.com" || host == L"api.github.com" || host == L"raw.githubusercontent.com" || host == L"release-assets.githubusercontent.com" || host == L"objects.githubusercontent.com", "Unexpected release download host");
        Internet connection{WinHttpConnect(session.h, host.c_str(), parts.nPort, 0)}; requireNetwork(connection.h, "HTTPS connection failed");
        Internet request{WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        require(request.h, "Cannot create HTTPS request"); DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        require(WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)), "Cannot set redirect policy");
        requireNetwork(WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request.h, nullptr), "HTTPS request failed; check your connection");
        DWORD status = 0, length = sizeof(status);
        require(WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX), "Missing HTTP status");
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            length = 0; WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &length, WINHTTP_NO_HEADER_INDEX);
            require(length > 0 && length < 32768, "Invalid download redirect");
            std::wstring location(length / sizeof(wchar_t), L'\0');
            require(WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location.data(), &length, WINHTTP_NO_HEADER_INDEX), "Missing redirect");
            location.resize(wcslen(location.c_str())); url = location; continue;
        }
        const auto httpError = "Release download returned HTTP " + std::to_string(status);
        requireNetwork(status == 200, httpError);
        wchar_t contentLength[32]{}; length = sizeof(contentLength);
        uint64_t expected = 0;
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                contentLength, &length, WINHTTP_NO_HEADER_INDEX)) expected = _wcstoui64(contentLength, nullptr, 10);
        require(expected <= limit, "Download exceeds allowed size");
        if (progress) progress(0, expected);
        std::vector<char> buffer(1024 * 1024); uint64_t total = 0;
        for (;;) {
            require(!cancelled(), "Update cancelled"); DWORD received = 0;
            requireNetwork(WinHttpReadData(request.h, buffer.data(), DWORD(buffer.size()), &received), "Download interrupted");
            if (!received) break; total += received; require(total <= limit, "Download exceeds allowed size");
            write(buffer.data(), received);
            if (progress) progress(total, expected);
        }
        requireNetwork(total > 0 && (!expected || total == expected), "Empty/incomplete download"); return;
    }
    throw NetworkError("Too many download redirects");
}
void download(const std::string& url, const fs::path& output, uint64_t limit,
              const std::function<bool()>& cancelled, const std::function<void(uint64_t,uint64_t)>& progress) {
    fs::create_directories(output.parent_path());
    std::ofstream out;
    transfer(url, [&](const char* data, size_t size) {
        if (!out.is_open()) out.open(output, std::ios::binary | std::ios::trunc);
        require(bool(out), "Cannot create download file");
        out.write(data, size); require(bool(out), "Download disk write failed");
    }, limit, cancelled, progress);
    out.close(); require(bool(out), "Download disk write failed");
}
std::string downloadText(const std::string& url, uint64_t limit, const std::function<bool()>& cancelled) {
    std::string result;
    transfer(url, [&](const char* data, size_t size) { result.append(data, size); }, limit,
        [&] { return cancelled && cancelled(); }, {});
    return result;
}
}
