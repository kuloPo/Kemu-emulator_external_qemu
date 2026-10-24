#include "UpdateCore.h"
#include <windows.h>
#include <iostream>
#include <functional>
using namespace kemu::update;
static void check(bool v, const char* message) { require(v, message); }
static void rejects(const std::function<void()>& fn) { bool failed = false; try { fn(); } catch (...) { failed = true; } check(failed, "Expected rejection"); }
static Json item(const fs::path& root, const fs::path& payload, const std::string& path) {
    return {{"path",path},{"sha256",sha256(payload/path)},{"previous_sha256",fs::exists(root/path) ? Json(sha256(root/path)) : Json(nullptr)}};
}
static fs::path setup(const fs::path& base, const std::string& name) {
    auto root = base / name, stage = root / ".kemu-update";
    fs::create_directories(stage / "payload"); writeText(root / "VERSION.txt", "v0.1.0\n"); writeText(root / "a.dll", "old");
    writeText(stage / "payload/a.dll", "new"); writeText(stage / "payload/new.dll", "added"); writeText(stage / "payload/VERSION.txt", "v0.2.0\n");
    writeJson(stage / "job.json", {{"schema_version",1},{"from","v0.1.0"},{"to","v0.2.0"},{"files",Json::array({item(root,stage/"payload","a.dll"),item(root,stage/"payload","new.dll"),item(root,stage/"payload","VERSION.txt")})}});
    writeJson(root / ".kemu-update-pending", {{"stage",".kemu-update"}}); return root;
}
int main(int argc, char** argv) {
    try {
        check(compareVersions("v0.10.0", "v0.2.0") > 0, "Numeric version ordering");
        check(compareVersions("v1.0.0-rc.2", "v1.0.0-rc.10") < 0, "Prerelease numeric ordering");
        check(compareVersions("v1.0.0", "v1.0.0-rc.10") > 0, "Release ordering");
        rejects([] { validateVersion("v01.2.3"); }); rejects([] { validateVersion("v0.1.0\nv0.2.0"); });
        for (const auto& path : {"../a", "a/../b", "C:/a", "/a", "a\\b", "nul.txt", "a:stream", "a.", ".kemu-update/payload", "a//b"}) rejects([&] { validatePath(path); });
        Json history = {{"schema_version",1},{"releases",Json::array({
            {{"version","v0.1.0"},{"files",Json::array()}},
            {{"version","v0.1.1"},{"files",{"/vendor/lib64/a.so"}}},
            {{"version","v0.1.2"},{"files",{"/vendor/lib64/a.so","/vendor/lib64/b.so"}}},
            {{"version","v0.2.0"},{"files",{"/vendor/lib64/b.so","KemuUpdater.exe"}}},
            {{"version","v0.3.0-beta"},{"files",Json::array()}}
        })}};
        auto plan = resolve(history,"v0.1.0"); check(plan.target == "v0.2.0", "Stable channel");
        check(plan.files.at("/vendor/lib64/a.so").version == "v0.1.2" && plan.files.at("/vendor/lib64/b.so").version == "v0.2.0", "Last writer selection");
        check(!plan.requiresFullInstaller, "Current empty release and future prerelease must not block updates");
        auto major = history;
        major["releases"][3]["files"] = Json::array();
        check(resolve(major,"v0.1.0").requiresFullInstaller, "Empty target requires full installer");
        check(!resolve(major,"v0.2.0").requiresFullInstaller, "Latest installed version must not require reinstall");
        major = history;
        major["releases"][1]["files"] = Json::array();
        auto blocked = resolve(major,"v0.1.0");
        check(blocked.requiresFullInstaller && blocked.files.empty() && blocked.target == "v0.2.0",
              "Intermediate empty release must block a later incremental update");
        check(!resolve(major,"v0.1.1").requiresFullInstaller, "Older empty release must not block updates");
        rejects([&] { resolve(history,"v0.0.0"); });
        auto duplicate = history; duplicate["releases"].push_back(history["releases"][0]); rejects([&] { resolve(duplicate,"v0.1.0"); });
        auto bad = history; bad["releases"][1]["files"] = {"VERSION.txt"}; rejects([&] { resolve(bad,"v0.1.0"); });
        rejects([] { parseChecksums(std::string(64,'a') + "  Windows/../bad\n"); });
        auto temp = fs::temp_directory_path() / ("kemu-update-tests-" + std::to_string(GetCurrentProcessId()));
        check(!fs::exists(temp), "Fresh test directory required"); fs::create_directory(temp);
        struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p,ec); } } cleanup{temp};
        auto root = setup(temp,"success"), stage = root/".kemu-update";
        apply(root,stage); check(readVersion(root)=="v0.2.0" && readText(root/"a.dll")=="new" && readText(stage/"backup/a.dll")=="old", "Successful installation and backup");
        check(!fs::exists(root/".kemu-update-pending"), "Commit marker removed");
        root=setup(temp,"tamper"); stage=root/".kemu-update"; writeText(stage/"payload/a.dll","tampered");
        rejects([&] { apply(root,stage); }); check(readVersion(root)=="v0.1.0" && readText(root/"a.dll")=="old", "Tamper must leave installation intact");
        root=setup(temp,"locked"); stage=root/".kemu-update";
        HANDLE lock=CreateFileW((root/"VERSION.txt").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        check(lock!=INVALID_HANDLE_VALUE,"Test lock"); rejects([&] { apply(root,stage); }); CloseHandle(lock);
        check(readVersion(root)=="v0.1.0" && readText(root/"a.dll")=="old" && !fs::exists(root/"new.dll"), "Rollback after partial apply");
        root=setup(temp,"interrupted"); stage=root/".kemu-update"; fs::create_directory(stage/"backup");
        writeJson(stage/"journal.json",{{"state","applying"},{"entries",Json::array({{{"path","a.dll"},{"existed",true},{"sha256",sha256(stage/"payload/a.dll")}}})}});
        fs::rename(root/"a.dll",stage/"backup/a.dll"); fs::rename(stage/"payload/a.dll",root/"a.dll"); recover(root,stage);
        check(readText(root/"a.dll")=="old" && readVersion(root)=="v0.1.0", "Crash recovery");
        recover(root,stage); check(readText(root/"a.dll")=="old", "Recovery idempotency");
        root=setup(temp,"changed-base"); stage=root/".kemu-update"; writeText(root/"a.dll","external edit"); rejects([&] { apply(root,stage); });
        check(readText(root/"a.dll")=="external edit", "Preserve concurrent changes");
        root=setup(temp,"downloaded"); stage=root/".kemu-update";
        Json cachedHistory = {{"schema_version",1},{"releases",Json::array({
            {{"version","v0.1.0"},{"files",Json::array()}},
            {{"version","v0.2.0"},{"files",{"a.dll"}}}
        })}};
        fs::remove(root/".kemu-update-pending");
        prepareUpdateTask(root,cachedHistory);
        writeText(stage/"payload/a.dll","new");
        writeText(stage/"retained.zip","completed package");
        prepareUpdateTask(root,cachedHistory);
        check(readText(stage/"retained.zip")=="completed package", "Same target must preserve interrupted task");
        writeText(stage/"v0.2.0/sha256sum.txt",sha256(stage/"payload/a.dll")+"  Windows/a.dll\n");
        auto cachedPlan=resolve(cachedHistory,"v0.1.0");
        saveDownloadedUpdate(stage,cachedPlan);
        fs::path restoredStage; Plan restoredPlan;
        check(loadDownloadedUpdate(root,restoredStage,restoredPlan) && restoredStage==stage && restoredPlan.target=="v0.2.0", "Downloaded update survives restart");
        check(loadDownloadedUpdate(root,restoredStage,restoredPlan), "Later choice preserves downloaded update");
        writeText(stage/"payload/a.dll","corrupt");
        check(!loadDownloadedUpdate(root,restoredStage,restoredPlan) && !fs::exists(stage/"downloaded.json"), "Corrupt cache rejected");
        writeText(stage/"payload/a.dll","new"); saveDownloadedUpdate(stage,cachedPlan);
        writeText(root/"VERSION.txt","v0.2.0\n");
        check(!loadDownloadedUpdate(root,restoredStage,restoredPlan), "Stale cache rejected after version change");
        writeText(root/"VERSION.txt","v0.1.0\n");
        auto newerHistory=cachedHistory;
        newerHistory["releases"].push_back({{"version","v0.3.0"},{"files",{"a.dll"}}});
        prepareUpdateTask(root,newerHistory);
        check(!fs::exists(stage/"retained.zip") && readJson(stage/"task.json").at("to")=="v0.3.0", "New latest target must clear previous task");
        writeText(stage/"protected","rollback"); writeJson(root/".kemu-update-pending",{{"stage",".kemu-update"}});
        rejects([&] { prepareUpdateTask(root,cachedHistory); });
        check(readText(stage/"protected")=="rollback", "Do not clear unfinished installation");
        rejects([&] { clearUpdateDirectory(root, root); });
        root = temp/"full-installer"; stage = root/".kemu-update";
        writeText(root/"VERSION.txt","v0.1.0\n");
        check(prepareUpdateTask(root,major).requiresFullInstaller && !fs::exists(stage),
              "Full installer must not create an incremental task");
        writeJson(stage/"history.json",major);
        writeJson(stage/"downloaded.json",{{"schema_version",1},{"from","v0.1.0"},{"to","v0.2.0"},{"files",Json::array()}});
        check(!loadDownloadedUpdate(root,restoredStage,restoredPlan), "Full installer must reject cached incremental updates");
        if (argc > 1) {
            fs::path fixtures=fs::u8path(argv[1]);
            extractPack(fixtures/"good.zip",temp/"good"); check(readText(temp/"good/Windows/a.dll")=="payload", "ZIP extraction");
            for (const auto& name : {"traversal", "bad-hash", "symlink", "case-collision", "missing-checksum"}) rejects([&] { extractPack(fixtures/(std::string(name)+".zip"),temp/name); });
        }
        std::cout << "Update protocol, integrity, transaction and recovery checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
