#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "nlohmann/json.hpp"

namespace kemu::update {
namespace fs = std::filesystem;
using Json = nlohmann::json;
inline constexpr const char* ReleasesUrl = "https://github.com/kuloPo/Kemu/releases";
inline constexpr const char* HistoryUrl = "https://raw.githubusercontent.com/kuloPo/Kemu/main/version-history.json";
inline constexpr const char* ApiReleasesUrl = "https://api.github.com/repos/kuloPo/Kemu/releases/tags/";
struct Release { std::string version; std::vector<std::string> files; };
struct Plan {
    std::string current, target;
    std::vector<Release> releases;
    std::map<std::string, Release> files;
    bool requiresFullInstaller = false;
};
struct Installer { std::string name, url, hash; uint64_t size = 0; };
struct VersionUnavailable : std::runtime_error { VersionUnavailable() : std::runtime_error("Installed version is unknown or unreadable") {} };
struct InstallerIntegrityError : std::runtime_error { using std::runtime_error::runtime_error; };
enum class FullInstallerCheck { Hash, MetadataOnly };
struct UpdateCancelled : std::runtime_error { UpdateCancelled() : std::runtime_error("Update cancelled") {} };
class Cancellation {
public:
    void reset() { mState = State::Active; }
    bool request();
    bool requested() const { return mState == State::Cancelled; }
    void check() const { if (requested()) throw UpdateCancelled(); }
    void beginCommit();
private:
    enum class State { Active, Cancelled, Committing };
    std::atomic<State> mState{State::Active};
};
using CancelCheck = std::function<void()>;
Installer selectInstaller(const Json& release, const std::string& version);
void validateInstaller(const fs::path& stage, const Installer& installer, const CancelCheck& checkCancelled = {});
class LockedInstaller {
public:
    LockedInstaller(const fs::path& stage, const Installer& installer);
    ~LockedInstaller();
    LockedInstaller(const LockedInstaller&) = delete;
    LockedInstaller& operator=(const LockedInstaller&) = delete;
private:
    std::vector<void*> mHandles;
};
Installer fetchInstaller(const std::string& version);
std::string downloadText(const std::string& url, uint64_t limit, const std::function<bool()>& cancelled = {});
bool cleanupInstalledFullUpdate(const fs::path& root, bool runningHelper = false);
using Download = std::function<void(const std::string&, const fs::path&, uint64_t,
    const std::function<bool()>&, const std::function<void(uint64_t, uint64_t)>&)>;
// A connection failure or a temporary server error; retrying later may succeed.
struct NetworkError : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool ok, const std::string& message);
void validateVersion(const std::string& version);
int compareVersions(const std::string& a, const std::string& b);
void validatePath(const std::string& path, bool android = false);
void validateHash(const std::string& hash);
Plan resolve(const Json& history, const std::string& current);
void clearUpdateDirectory(const fs::path& root, const fs::path& directory);
Plan prepareUpdateTask(const fs::path& root, const Json& history);
void saveDownloadedUpdate(const fs::path& stage, const Plan& plan,
                          const CancelCheck& checkCancelled = {}, const std::function<void()>& beginCommit = {});
// MetadataOnly defers installer content verification to LockedInstaller before launch.
bool loadDownloadedUpdate(const fs::path& root, fs::path& stage, Plan& plan,
                          FullInstallerCheck installerCheck = FullInstallerCheck::Hash);
std::string sha256(const fs::path& path, const CancelCheck& checkCancelled = {});
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
