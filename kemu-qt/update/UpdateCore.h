#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "nlohmann/json.hpp"

namespace kemu::update {
namespace fs = std::filesystem;
using Json = nlohmann::json;
inline constexpr const char* ReleasesUrl = "https://github.com/kuloPo/Kemu/releases";
inline constexpr const char* HistoryUrl = "https://raw.githubusercontent.com/kuloPo/Kemu/main/version-history.json";
struct Release { std::string version; std::vector<std::string> files; };
struct Plan {
    std::string current, target;
    std::map<std::string, Release> files;
    bool requiresFullInstaller = false;
};
void require(bool ok, const std::string& message);
void validateVersion(const std::string& version);
int compareVersions(const std::string& a, const std::string& b);
void validatePath(const std::string& path, bool android = false);
void validateHash(const std::string& hash);
Plan resolve(const Json& history, const std::string& current);
void clearUpdateDirectory(const fs::path& root, const fs::path& directory);
Plan prepareUpdateTask(const fs::path& root, const Json& history);
void saveDownloadedUpdate(const fs::path& stage, const Plan& plan);
bool loadDownloadedUpdate(const fs::path& root, fs::path& stage, Plan& plan);
std::string sha256(const fs::path& path);
std::string readText(const fs::path& path, size_t limit = 8 * 1024 * 1024);
std::string readVersion(const fs::path& root);
void writeText(const fs::path& path, const std::string& data);
void writeJson(const fs::path& path, const Json& value);
Json readJson(const fs::path& path);
void safePath(const fs::path& root, const std::string& relative);
std::map<std::string, std::string> parseChecksums(const std::string& text);
void extractPack(const fs::path& archive, const fs::path& output);
void download(const std::string& url, const fs::path& output, uint64_t limit,
              const std::function<bool()>& cancelled,
              const std::function<void(uint64_t, uint64_t)>& progress = {});
// Apply a prepared job after the parent exits. A persistent journal allows recovery.
void apply(const fs::path& root, const fs::path& stage);
void recover(const fs::path& root, const fs::path& stage);
void validateJob(const fs::path& root, const fs::path& stage);
}
