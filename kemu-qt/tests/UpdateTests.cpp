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
static void checkDownloadCancellation(const fs::path& base) {
    for (const bool full : {false, true}) {
        const auto root = base/(full ? "cancel-full" : "cancel-delta"), stage = root/".kemu-update";
        writeText(root/"VERSION.txt", "v0.1.0\n");
        const Json history = {{"schema_version",1},{"releases",Json::array({
            {{"version","v0.1.0"},{"files",Json::array()}},
            {{"version","v0.2.0"},{"files",full ? Json::array() : Json::array({"a.dll"})}}})}};
        const auto plan = prepareUpdateTask(root,history);
        const auto file = stage/(full ? "Kemu-v0.2.0-Setup-x64.exe" : "payload/a.dll");
        writeText(file,std::string(8*1024*1024,'x'));
        const auto digest = sha256(file);
        if (full) writeJson(stage/"installer-release.json",{{"tag_name","v0.2.0"},{"draft",false},{"assets",Json::array({{
            {"name",file.filename().u8string()},{"browser_download_url",std::string(ReleasesUrl)+"/download/v0.2.0/"+file.filename().u8string()},
            {"size",fs::file_size(file)},{"digest","sha256:"+digest}}})}});
        else writeText(stage/"v0.2.0/sha256sum.txt",digest+"  Windows/a.dll\n");
        Cancellation cancellation;
        int checks = 0; bool cancelled = false, committed = false;
        try {
            saveDownloadedUpdate(stage,plan,[&] {
                if (++checks == 5) cancellation.request();
                cancellation.check();
            },[&] { committed = true; cancellation.beginCommit(); });
        } catch (const UpdateCancelled&) { cancelled = true; }
        check(cancelled && checks == 5 && !committed && !fs::exists(stage/"downloaded.json"),
              "Cancellation inside the hash loop must abort before publishing a ready update");
        check(sha256(file)==digest,"Cancelling verification must preserve the downloaded cache");
        cancellation.reset(); cancelled = false;
        try {
            saveDownloadedUpdate(stage,plan,[&] { cancellation.check(); },[&] {
                cancellation.request(); cancellation.beginCommit();
            });
        } catch (const UpdateCancelled&) { cancelled = true; }
        check(cancelled && !fs::exists(stage/"downloaded.json"),"Cancellation just before commit must prevent a ready marker");
        cancellation.reset();
        saveDownloadedUpdate(stage,plan,[&] { cancellation.check(); },[&] {
            cancellation.beginCommit();
            check(!cancellation.request(),"Cancellation must not override an update that has entered commit");
        });
        fs::path restoredStage; Plan restoredPlan;
        check(!cancellation.requested() && loadDownloadedUpdate(root,restoredStage,restoredPlan),
              "A committed update must remain ready after a late cancellation request");
    }
}
static void checkInstallerFinalization(const fs::path& base) {
    for (const std::string source : {"download", "cache", "repair"}) {
        const auto root = base/("hash-"+source), stage = root/".kemu-update";
        writeText(root/"VERSION.txt","v0.1.0\n");
        const Json history = {{"schema_version",1},{"releases",Json::array({
            {{"version","v0.1.0"},{"files",Json::array()}},{{"version","v0.2.0"},{"files",Json::array()}}})}};
        const auto plan = prepareUpdateTask(root,history);
        const auto name = "Kemu-v0.2.0-Setup-x64.exe";
        const auto file = stage/name, partial = stage/(std::string(name)+".part");
        const auto input = source == "cache" ? file : partial;
        writeText(input,"installer payload");
        const auto digest = sha256(input);
        if (source == "repair") writeText(file,"corrupt installer");
        writeJson(stage/"installer-release.json",{{"tag_name","v0.2.0"},{"draft",false},{"assets",Json::array({{
            {"name",name},{"browser_download_url",std::string(ReleasesUrl)+"/download/v0.2.0/"+name},
            {"size",fs::file_size(input)},{"digest","sha256:"+digest}}})}});
        int hashPasses = 0; bool hashing = false;
        saveDownloadedUpdate(stage,plan,[&] {
            // Each content hash holds a read handle that excludes writers.
            bool locked = false;
            for (const auto& candidate : {file, partial}) {
                if (!fs::exists(candidate)) continue;
                HANDLE probe = CreateFileW(candidate.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                    nullptr,OPEN_EXISTING,0,nullptr);
                if (probe == INVALID_HANDLE_VALUE) {
                    check(GetLastError() == ERROR_SHARING_VIOLATION,"Unexpected hash probe error"); locked = true;
                } else CloseHandle(probe);
            }
            if (locked && !hashing) ++hashPasses;
            hashing = locked;
        });
        check(hashPasses == (source == "repair" ? 2 : 1),"Installer finalization must hash each candidate only once");
        check(sha256(file)==digest && !fs::exists(partial) && fs::exists(stage/"downloaded.json"),
              "One successful verification must promote the installer and persist its ready state");
    }
}
static void checkUnknownVersionCache(const fs::path& base) {
    for (const bool full : {false, true}) {
        const auto root = base/(full ? "unknown-full" : "unknown-delta"), stage = root/".kemu-update";
        writeText(root/"VERSION.txt","v0.1.0\n");
        const Json history = {{"schema_version",1},{"releases",Json::array({
            {{"version","v0.1.0"},{"files",Json::array()}},
            {{"version","v0.2.0"},{"files",full ? Json::array() : Json::array({"a.dll"})}}})}};
        const auto plan = prepareUpdateTask(root,history);
        writeText(stage/"downloaded.json","preserve even an unreadable marker");
        writeText(stage/"payload/a.dll","cached payload"); writeText(stage/"v0.2.0.tar.xz","cached archive");
        auto snapshot = [&] {
            std::map<std::string,std::pair<std::string,fs::file_time_type>> result;
            for (const auto& entry : fs::recursive_directory_iterator(stage)) if (entry.is_regular_file())
                result.emplace(entry.path().u8string(),std::make_pair(readText(entry.path()),entry.last_write_time()));
            return result;
        };
        const auto before = snapshot();
        for (const std::string state : {"missing","invalid","locked"}) {
            HANDLE versionLock = INVALID_HANDLE_VALUE;
            if (state=="missing") fs::remove(root/"VERSION.txt");
            else writeText(root/"VERSION.txt",state=="invalid" ? "unknown" : "v0.1.0\n");
            if (state=="locked") {
                versionLock=CreateFileW((root/"VERSION.txt").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
                check(versionLock!=INVALID_HANDLE_VALUE,"Cannot lock the version fixture");
            }
            int skipped = 0; fs::path restoredStage; Plan restoredPlan;
            try { prepareUpdateTask(root,history); } catch (const VersionUnavailable&) { ++skipped; }
            try { loadDownloadedUpdate(root,restoredStage,restoredPlan); } catch (const VersionUnavailable&) { ++skipped; }
            try { cleanupInstalledFullUpdate(root); } catch (const VersionUnavailable&) { ++skipped; }
            try { saveDownloadedUpdate(stage,plan); } catch (const VersionUnavailable&) { ++skipped; }
            if (versionLock!=INVALID_HANDLE_VALUE) CloseHandle(versionLock);
            check(skipped==4 && snapshot()==before && !fs::exists(root/".kemu-update-lock"),
                  "Unknown versions must not prepare, invalidate, clean or finalize cached updates");
        }
    }
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
        check(plan.releases.size() == 3 && plan.releases.front().version == "v0.1.1" && plan.releases.back().version == "v0.2.0",
              "Every incremental package must be downloaded in version order, including overwritten files");
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
        check(resolve(history,"v0.0.0").requiresFullInstaller, "Known versions absent from history need the latest full installer");
        rejects([&] { resolve(history,""); });
        auto unordered = history;
        std::reverse(unordered["releases"].begin(), unordered["releases"].end());
        check(resolve(unordered,"v0.1.0").releases.front().version == "v0.1.1", "History order must not affect update order");
        auto duplicate = history; duplicate["releases"].push_back(history["releases"][0]); rejects([&] { resolve(duplicate,"v0.1.0"); });
        auto bad = history; bad["releases"][1]["files"] = {"VERSION.txt"}; rejects([&] { resolve(bad,"v0.1.0"); });
        rejects([] { parseChecksums(std::string(64,'a') + "  Windows/../bad\n"); });
        auto temp = fs::temp_directory_path() / ("kemu-update-tests-" + std::to_string(GetCurrentProcessId()));
        check(!fs::exists(temp), "Fresh test directory required"); fs::create_directory(temp);
        struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p,ec); } } cleanup{temp};
        checkDownloadCancellation(temp);
        checkInstallerFinalization(temp);
        checkUnknownVersionCache(temp);
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
        writeText(stage/"retained.tar.xz","completed package");
        prepareUpdateTask(root,cachedHistory);
        check(readText(stage/"retained.tar.xz")=="completed package", "Same target must preserve interrupted task");
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
        check(!fs::exists(stage/"retained.tar.xz") && readJson(stage/"task.json").at("to")=="v0.3.0", "New latest target must clear previous task");
        writeText(stage/"protected","rollback"); writeJson(root/".kemu-update-pending",{{"stage",".kemu-update"}});
        rejects([&] { prepareUpdateTask(root,cachedHistory); });
        check(readText(stage/"protected")=="rollback", "Do not clear unfinished installation");
        rejects([&] { clearUpdateDirectory(root, root); });
        root = temp/"full-installer"; stage = root/".kemu-update";
        writeText(root/"VERSION.txt","v0.1.0\n");
        check(prepareUpdateTask(root,major).requiresFullInstaller && fs::exists(stage/"task.json"),
              "Full installer needs a persistent download task");
        writeJson(stage/"history.json",major);
        writeJson(stage/"downloaded.json",{{"schema_version",1},{"from","v0.1.0"},{"to","v0.2.0"},{"files",Json::array()}});
        check(!loadDownloadedUpdate(root,restoredStage,restoredPlan), "Full installer must reject cached incremental updates");
        const auto name = "Kemu-v0.2.0-Setup-x64.exe";
        writeText(stage/name,"installer payload");
        Json metadata = {{"tag_name","v0.2.0"},{"draft",false},{"assets",Json::array({{
            {"name",name},{"browser_download_url",std::string(ReleasesUrl)+"/download/v0.2.0/"+name},
            {"size",fs::file_size(stage/name)},{"digest","sha256:"+sha256(stage/name)}}})}};
        writeJson(stage/"installer-release.json",metadata);
        const auto fullPlan = resolve(major,"v0.1.0");
        saveDownloadedUpdate(stage,fullPlan);
        check(loadDownloadedUpdate(root,restoredStage,restoredPlan) && restoredPlan.requiresFullInstaller,
              "Full installer survives Later and restart");
        {
            LockedInstaller locked(stage, selectInstaller(metadata,"v0.2.0"));
            HANDLE writer = CreateFileW((stage/name).c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
            if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
            check(writer == INVALID_HANDLE_VALUE, "Verified installer must reject writes");
            check(!DeleteFileW((stage/name).c_str()), "Verified installer must reject deletion");
            check(!MoveFileW((stage/name).c_str(),(stage/"replaced.exe").c_str()), "Verified installer must reject replacement");
            check(!MoveFileW(stage.c_str(),(root/"moved-stage").c_str()), "Verified installer directory must reject replacement");
            check(!MoveFileW(root.c_str(),(temp/"moved-root").c_str()), "Installer ancestors must reject replacement");
        }
        auto invalid = metadata; invalid["tag_name"] = "v0.1.0";
        rejects([&] { selectInstaller(invalid,"v0.2.0"); });
        invalid = metadata; invalid["assets"][0]["digest"] = nullptr;
        rejects([&] { selectInstaller(invalid,"v0.2.0"); });
        invalid = metadata; invalid["assets"][0]["browser_download_url"] = "https://example.com/setup.exe";
        rejects([&] { selectInstaller(invalid,"v0.2.0"); });
        invalid = metadata; invalid["assets"] = Json::array();
        rejects([&] { selectInstaller(invalid,"v0.2.0"); });
        invalid = metadata; invalid["assets"].push_back(invalid["assets"][0]);
        rejects([&] { selectInstaller(invalid,"v0.2.0"); });
        writeText(stage/name,"corrupt installer");
        check(fs::file_size(stage/name)==metadata["assets"][0]["size"].get<uint64_t>(),"Tamper fixture must retain its size");
        check(loadDownloadedUpdate(root,restoredStage,restoredPlan,FullInstallerCheck::MetadataOnly),
              "Installation preflight must defer content hashing to the locked launcher check");
        rejects([&] { LockedInstaller locked(stage, selectInstaller(metadata,"v0.2.0")); });
        check(!loadDownloadedUpdate(root,restoredStage,restoredPlan), "Changed installer must not be offered or launched");
        rejects([&] { saveDownloadedUpdate(stage,fullPlan); });
        writeText(stage/name,"installer payload"); saveDownloadedUpdate(stage,fullPlan);
        writeText(stage/"KemuInstallerLauncher.exe","running helper");
        cleanupInstalledFullUpdate(root);
        check(fs::exists(stage/name), "Uninstalled or cancelled updates must retain the installer");
        writeText(root/"VERSION.txt","v0.2.0\n");
        lock=CreateFileW((root/".kemu-update-lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr);
        check(lock!=INVALID_HANDLE_VALUE,"Cleanup test lock");
        check(!cleanupInstalledFullUpdate(root), "Startup cleanup must wait for a running installer helper");
        cleanupInstalledFullUpdate(root,true); CloseHandle(lock);
        check(!fs::exists(stage/name) && !fs::exists(stage/"downloaded.json") &&
              fs::exists(stage/"KemuInstallerLauncher.exe") && fs::exists(stage/"task.json"),
              "Successful installation must remove the installer and retain only the running helper task");
        check(cleanupInstalledFullUpdate(root) && !fs::exists(stage), "Next startup must remove the helper cache");
        writeText(root/"VERSION.txt","v0.1.0\n");
        prepareUpdateTask(root,cachedHistory);
        check(!fs::exists(stage/name), "Changing update type must clear the old task");
        if (argc > 1) {
            fs::path fixtures=fs::u8path(argv[1]);
            extractPack(fixtures/"good.tar.xz",temp/"good"); check(readText(temp/"good/Windows/a.dll")=="payload", "Pack extraction");
            extractPack(fixtures/"large.tar.xz",temp/"large"); check(fs::file_size(temp/"large/Android/vendor/lib64/big.so")==3*1024*1024 && fs::file_size(temp/"large/Windows/empty.dll")==0, "Streamed pack extraction");
            for (const std::string name : {"traversal", "bad-hash", "symlink", "case-collision", "missing-checksum", "hardlink", "pax", "gnu", "header-checksum",
                                           "data-after-end", "corrupt", "truncated", "trailing-garbage", "missing-end", "one-end-block"})
                rejects([&] { extractPack(fixtures/(name+".tar.xz"),temp/name); });
            writeText(temp/"unknown.pack","not a pack"); rejects([&] { extractPack(temp/"unknown.pack",temp/"unknown"); });
        }
        std::cout << "Update protocol, integrity, transaction and recovery checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
