#include "UpdateCore.h"
#include <windows.h>
#include <iostream>
using namespace kemu::update;
static std::wstring quote(const fs::path& p) { return L"\""+p.wstring()+L"\""; }
static HANDLE launch(const fs::path& exe, std::wstring args) {
    auto command=quote(exe)+L" "+args; STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&si,&pi),"Cannot launch handoff process");
    CloseHandle(pi.hThread); return pi.hProcess;
}
static void waitProcess(HANDLE process, DWORD expected = 0) {
    require(WaitForSingleObject(process,60000)==WAIT_OBJECT_0,"Test process did not exit");
    DWORD code=1; GetExitCodeProcess(process,&code); CloseHandle(process); require(code==expected,"Test process failed");
}
static void checkInstallerLocked(const fs::path& root) {
    const auto stage=root/".kemu-update", installer=stage/"Kemu-v0.2.0-Setup-x64.exe";
    HANDLE file=CreateFileW(installer.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if (file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    require(file==INVALID_HANDLE_VALUE && !DeleteFileW(installer.c_str()),"Verified installer became writable or deletable");
    require(!MoveFileW(stage.c_str(),(root/"replaced-stage").c_str()),"Installer directory became replaceable");
}
struct ErrorWindow { DWORD pid; HWND window = nullptr; };
static BOOL CALLBACK findError(HWND window, LPARAM parameter) {
    auto& result=*reinterpret_cast<ErrorWindow*>(parameter);
    DWORD pid=0; GetWindowThreadProcessId(window,&pid);
    wchar_t type[32]{}; GetClassNameW(window,type,32);
    wchar_t title[64]{}; GetWindowTextW(window,title,64);
    if (pid==result.pid && std::wstring(type)==L"#32770" && std::wstring(title)==L"Kemu Update" && IsWindowVisible(window)) result.window=window;
    return TRUE;
}
int wmain(int argc,wchar_t** argv) {
    try {
        std::wstring module(32768,L'\0'); module.resize(GetModuleFileNameW(nullptr,module.data(),DWORD(module.size()))); fs::path self(module);
        if (self.filename()==L"Kemu.exe") {
            const auto root=self.parent_path();
            if (argc==2 && std::wstring(argv[1])==L"--test-parent")
                waitProcess(launch(root/"qemu/windows-x86_64/qemu-system-x86_64.exe",L""));
            else {
                if (readText(root/"scenario")=="launch-error") {
                    require(argc==3 && std::wstring(argv[1])==L"--restart-pid","Failure restart must wait for the old helper");
                    HANDLE helper=OpenProcess(SYNCHRONIZE,FALSE,std::stoul(argv[2]));
                    if (helper) { require(WaitForSingleObject(helper,10000)==WAIT_OBJECT_0,"Restart did not release the old helper"); CloseHandle(helper); }
                    HANDLE writable=CreateFileW((root/".kemu-update/KemuInstallerLauncher.exe").c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
                    require(writable!=INVALID_HANDLE_VALUE,"Restart retained the helper executable lock");
                    CloseHandle(writable);
                }
                writeText(root/"restarted","yes\n");
            }
            return 0;
        }
        if (self.filename()==L"Kemu-v0.2.0-Setup-x64.exe") {
            const auto root=self.parent_path().parent_path();
            const auto pid=std::stoul(readText(root/"parent-pid"));
            HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,pid);
            require(!parent || WaitForSingleObject(parent,0)==WAIT_OBJECT_0,"Installer launched before the emulator exited");
            if (parent) CloseHandle(parent);
            require(argc==4 && std::wstring(argv[1])==L"/DIR="+root.wstring() &&
                    std::wstring(argv[2])==L"/NOFORCECLOSEAPPLICATIONS" && std::wstring(argv[3])==L"/NORESTART",
                    "Installer must receive the current installation directory");
            checkInstallerLocked(root);
            HANDLE helper=CreateFileW((root/"KemuUpdater.exe").c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
            require(helper!=INVALID_HANDLE_VALUE,"Installed updater must not be locked by the installer handoff");
            CloseHandle(helper);
            const bool cancelled=readText(root/"scenario")=="cancel";
            if (!cancelled) writeText(root/"VERSION.txt","v0.2.0\n");
            writeText(root/"installer-opened","yes\n"); return cancelled ? 2 : 0;
        }
        if (self.filename()==L"qemu-system-x86_64.exe") {
            const auto root=self.parent_path().parent_path().parent_path(), stage=root/".kemu-update";
            const auto scenario=readText(root/"scenario");
            const bool full=scenario!="delta", rejected=scenario=="network" || scenario=="tamper" || scenario=="changed";
            writeText(root/"parent-pid",std::to_string(GetCurrentProcessId()));
            auto process=launch(full ? stage/"KemuInstallerLauncher.exe" : root/"KemuUpdater.exe",
                std::wstring(full ? L"--install " : L"--apply ")+quote(stage)+L" "+std::to_wstring(GetCurrentProcessId()));
            writeText(root/"helper-pid",std::to_string(GetProcessId(process)));
            if (rejected) {
                waitProcess(process,1);
                require(fs::exists(stage/"installer-error.json") && !fs::exists(stage/"installer-ready") &&
                        !fs::exists(root/"installer-opened"),"Failed verification must keep the emulator running");
                return 0;
            }
            CloseHandle(process);
            const auto ready=stage/(full ? "installer-ready" : "updater-ready");
            for(int i=0;i<300 && !fs::exists(ready);++i) Sleep(100);
            require(fs::exists(ready),"Updater did not acknowledge parent");
            require(readVersion(root)=="v0.1.0" && !fs::exists(root/"installer-opened"),"Updater acted before parent exit");
            if (full) checkInstallerLocked(root);
            return 0;
        }
        require(argc==3,"Pass the compiled KemuUpdater.exe and kemu-updater-test-helper.exe paths");
        for (const std::string scenario : {"delta","full","cancel","network","tamper","changed","launch-error"}) {
            std::cout << "Checking " << scenario << std::endl;
            const bool full=scenario!="delta", rejected=scenario=="network" || scenario=="tamper" || scenario=="changed";
            auto root=fs::temp_directory_path()/("kemu handoff "+std::to_string(GetCurrentProcessId())+" "+scenario);
            require(!fs::exists(root),"Fresh handoff directory required");
            const auto stage=root/".kemu-update", engine=root/"qemu/windows-x86_64/qemu-system-x86_64.exe";
            fs::create_directories(stage/"payload"); fs::create_directories(engine.parent_path());
            fs::copy_file(argv[1],root/"KemuUpdater.exe"); fs::copy_file(self,engine); fs::copy_file(self,root/"Kemu.exe");
            writeText(root/"VERSION.txt","v0.1.0\n"); writeText(root/"a.dll","old"); writeText(root/"scenario",scenario);
            if (full) {
                const Json history={{"schema_version",1},{"releases",Json::array({
                    {{"version","v0.1.0"},{"files",Json::array()}},{{"version","v0.2.0"},{"files",Json::array()}}})}};
                const auto plan=prepareUpdateTask(root,history);
                const auto name="Kemu-v0.2.0-Setup-x64.exe";
                if (scenario=="launch-error") writeText(stage/name,"invalid executable");
                else fs::copy_file(self,stage/name);
                fs::copy_file(argv[2],stage/"KemuInstallerLauncher.exe");
                Json metadata={{"tag_name","v0.2.0"},{"draft",false},{"assets",Json::array({{
                    {"name",name},{"browser_download_url",std::string(ReleasesUrl)+"/download/v0.2.0/"+name},
                    {"size",fs::file_size(stage/name)},{"digest","sha256:"+sha256(stage/name)}}})}};
                writeJson(root/"fixture-release.json",metadata);
                if (scenario=="tamper") {
                    writeText(stage/name,"replaced installer");
                    metadata["assets"][0]["size"]=fs::file_size(stage/name);
                    metadata["assets"][0]["digest"]="sha256:"+sha256(stage/name);
                }
                if (scenario=="network") writeText(root/"fixture-network-error","offline");
                writeJson(stage/"installer-release.json",metadata); saveDownloadedUpdate(stage,plan);
                if (scenario=="changed") writeText(stage/name,std::string(size_t(fs::file_size(stage/name)),'x'));
            } else {
                writeText(stage/"payload/VERSION.txt","v0.2.0\n"); writeText(stage/"payload/a.dll","new");
                Json entries=Json::array();
                for (const auto* name:{"a.dll","VERSION.txt"}) entries.push_back({{"path",name},{"sha256",sha256(stage/"payload"/name)},{"previous_sha256",sha256(root/name)}});
                writeJson(stage/"job.json",{{"schema_version",1},{"from","v0.1.0"},{"to","v0.2.0"},{"files",entries}});
                writeJson(root/".kemu-update-pending",{{"stage",".kemu-update"}});
            }
            waitProcess(launch(root/"Kemu.exe",L"--test-parent"));
            const DWORD helperPid=std::stoul(readText(root/"helper-pid"));
            HANDLE helper=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,helperPid);
            if (scenario=="launch-error") {
                ErrorWindow error{helperPid};
                for(int i=0;i<100 && !error.window;++i) { EnumWindows(findError,reinterpret_cast<LPARAM>(&error)); Sleep(100); }
                require(error.window && !fs::exists(root/"restarted"),"Failure dialog must precede restart");
                require(PostMessageW(error.window,WM_CLOSE,0,0),"Cannot close the test error message");
            }
            if (helper) waitProcess(helper,rejected || scenario=="launch-error" ? 1 : 0);
            if (scenario=="delta" || scenario=="launch-error") {
                for(int i=0;i<100 && !fs::exists(root/"restarted");++i) Sleep(100);
                require(fs::exists(root/"restarted"),"Update recovery did not restart Kemu");
            }
            if (scenario=="full") {
                require(readVersion(root)=="v0.2.0" && !fs::exists(stage/"Kemu-v0.2.0-Setup-x64.exe") &&
                        fs::exists(stage/"KemuInstallerLauncher.exe"),"Successful installer must clean its payload");
                require(cleanupInstalledFullUpdate(root) && !fs::exists(stage),"Next startup must remove the helper cache");
            } else if (full) {
                require(readVersion(root)=="v0.1.0" && fs::exists(stage/"downloaded.json"),"Failed or cancelled installers must retain a retryable download");
            } else require(readVersion(root)=="v0.2.0" && readText(root/"a.dll")=="new" && readText(stage/"backup/a.dll")=="old","Incremental installation failed");
            require(!fs::exists(root/".kemu-update-pending"),"Update marker was left behind");
            std::error_code ec; fs::remove_all(root,ec); require(!ec,"Handoff left a locked fixture");
        }
        std::cout<<"Incremental and full installer handoff, integrity, recovery and cleanup checks passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
