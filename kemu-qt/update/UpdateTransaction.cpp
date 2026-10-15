#include "UpdateCore.h"
#include <windows.h>
#include <set>

namespace kemu::update {
static void move(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    require(MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Cannot replace file (Windows " + std::to_string(GetLastError()) + "): " + to.u8string());
}
static void validateLocation(const fs::path& root, const fs::path& stage) {
    require(fs::is_directory(root) && fs::is_directory(stage) && fs::weakly_canonical(stage) == fs::weakly_canonical(root / ".kemu-update"), "Invalid update staging directory");
    require(!(GetFileAttributesW(stage.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) && !(GetFileAttributesW(stage.parent_path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "Unsafe update staging directory");
}
void validateJob(const fs::path& root, const fs::path& stage) {
    validateLocation(root, stage); auto job = readJson(stage / "job.json");
    require(job.at("schema_version") == 1, "Unsupported install job");
    validateVersion(job.at("from").get<std::string>()); validateVersion(job.at("to").get<std::string>());
    require(compareVersions(job.at("to"), job.at("from")) > 0, "Update does not advance version");
    require(readVersion(root) == job.at("from").get<std::string>(), "Installed version changed during preparation");
    require(job.at("files").is_array() && !job.at("files").empty(), "Empty install job");
    std::set<std::string> targets; bool image = false, params = false;
    for (const auto& entry : job.at("files")) {
        auto path = entry.at("path").get<std::string>(); safePath(root, path); safePath(stage / "payload", path);
        auto key = path; for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
        require(key != "kemuupdater.exe" && targets.insert(key).second, "Duplicate/reserved installation target");
        auto expected = entry.at("sha256").get<std::string>(); validateHash(expected);
        require(sha256(stage / "payload" / fs::u8path(path)) == expected, "Prepared payload hash mismatch: " + path);
        bool exists = fs::exists(root / fs::u8path(path));
        require(exists == !entry.at("previous_sha256").is_null(), "Installed file existence changed: " + path);
        if (exists) require(sha256(root / fs::u8path(path)) == entry.at("previous_sha256").get<std::string>(), "Installed file changed: " + path);
        image |= path == "system-img/system.img"; params |= path == "system-img/VerifiedBootParams.textproto";
    }
    require(image == params, "Image and verified boot parameters must be installed together");
    require(job.at("files").back().at("path") == "VERSION.txt" && readVersion(stage / "payload") == job.at("to").get<std::string>(), "VERSION.txt must be installed last");
}
void recover(const fs::path& root, const fs::path& stage) {
    validateLocation(root, stage);
    auto journalPath = stage / "journal.json";
    if (!fs::exists(journalPath)) { fs::remove(root / ".kemu-update-pending"); return; }
    auto journal = readJson(journalPath);
    if (journal.at("state") == "committed" || journal.at("state") == "rolled_back") { fs::remove(root / ".kemu-update-pending"); return; }
    auto& entries = journal.at("entries");
    for (auto i = entries.rbegin(); i != entries.rend(); ++i) {
        auto path = i->at("path").get<std::string>(); safePath(root, path); safePath(stage / "backup", path);
        auto destination = root / fs::u8path(path), backup = stage / "backup" / fs::u8path(path);
        if (fs::exists(backup)) move(backup, destination);
        else if (!i->at("existed").get<bool>() && fs::exists(destination)) {
            require(sha256(destination) == i->at("sha256").get<std::string>(), "Recovery refuses to delete a changed file: " + path);
            fs::remove(destination);
        }
    }
    journal["state"] = "rolled_back"; writeJson(journalPath, journal); fs::remove(root / ".kemu-update-pending");
}
void apply(const fs::path& root, const fs::path& stage) {
    validateJob(root, stage);
    require(!fs::exists(stage / "journal.json"), "Update job has already been applied; prepare a new job");
    fs::create_directories(stage / "backup");
    auto job = readJson(stage / "job.json"); Json journal = {{"state", "applying"}, {"entries", Json::array()}};
    writeJson(stage / "journal.json", journal);
    try {
        for (const auto& entry : job.at("files")) {
            auto path = entry.at("path").get<std::string>();
            auto target = root / fs::u8path(path), source = stage / "payload" / fs::u8path(path), backup = stage / "backup" / fs::u8path(path);
            safePath(root, path); safePath(stage / "payload", path); safePath(stage / "backup", path);
            journal["entries"].push_back({{"path", path}, {"existed", fs::exists(target)}, {"sha256", entry.at("sha256")}});
            writeJson(stage / "journal.json", journal);
            if (fs::exists(target)) move(target, backup);
            move(source, target);
            require(sha256(target) == entry.at("sha256").get<std::string>(), "Installed file verification failed");
        }
        journal["state"] = "committed"; writeJson(stage / "journal.json", journal); fs::remove(root / ".kemu-update-pending");
    } catch (...) { recover(root, stage); throw; }
}
}
