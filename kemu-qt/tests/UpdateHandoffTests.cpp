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
int wmain(int argc,wchar_t** argv) {
    try {
        std::wstring module(32768,L'\0'); module.resize(GetModuleFileNameW(nullptr,module.data(),DWORD(module.size()))); fs::path self(module);
        if (self.filename()==L"Kemu.exe") { writeText(self.parent_path()/"restarted","yes\n"); return 0; }
        if (self.filename()==L"qemu-system-x86_64.exe") {
            auto root=self.parent_path().parent_path().parent_path(), stage=root/".kemu-update";
            auto process=launch(root/"KemuUpdater.exe",L"--apply "+quote(stage)+L" "+std::to_wstring(GetCurrentProcessId())); CloseHandle(process);
            for(int i=0;i<300 && !fs::exists(stage/"updater-ready");++i) Sleep(100);
            require(fs::exists(stage/"updater-ready"),"Updater did not acknowledge parent");
            require(readVersion(root)=="v0.1.0","Updater replaced files before parent exit");
            return 0;
        }
        require(argc==2,"Pass the compiled KemuUpdater.exe path");
        auto root=fs::temp_directory_path()/("kemu-handoff-"+std::to_string(GetCurrentProcessId()));
        require(!fs::exists(root),"Fresh handoff directory required");
        auto stage=root/".kemu-update", engine=root/"qemu/windows-x86_64/qemu-system-x86_64.exe";
        fs::create_directories(stage/"payload"); fs::create_directories(engine.parent_path());
        fs::copy_file(argv[1],root/"KemuUpdater.exe"); fs::copy_file(self,engine); fs::copy_file(self,root/"Kemu.exe");
        writeText(root/"VERSION.txt","v0.1.0\n"); writeText(root/"a.dll","old");
        writeText(stage/"payload/VERSION.txt","v0.2.0\n"); writeText(stage/"payload/a.dll","new");
        Json entries=Json::array();
        for (const auto* name:{"a.dll","VERSION.txt"}) entries.push_back({{"path",name},{"sha256",sha256(stage/"payload"/name)},{"previous_sha256",sha256(root/name)}});
        writeJson(stage/"job.json",{{"schema_version",1},{"from","v0.1.0"},{"to","v0.2.0"},{"files",entries}});
        writeJson(root/".kemu-update-pending",{{"stage",".kemu-update"}});
        auto parent=launch(engine,L""); require(WaitForSingleObject(parent,60000)==WAIT_OBJECT_0,"Test parent did not exit");
        DWORD code=1; GetExitCodeProcess(parent,&code); CloseHandle(parent); require(code==0,"Parent handoff failed");
        for(int i=0;i<300 && !fs::exists(root/"restarted");++i) Sleep(100);
        require(fs::exists(root/"restarted") && readVersion(root)=="v0.2.0" && readText(root/"a.dll")=="new","Restart/installation failed");
        require(readText(stage/"backup/a.dll")=="old" && !fs::exists(root/".kemu-update-pending"),"Commit cleanup/backup failed");
        std::cout<<"Actual updater parent handoff, installation and restart passed\n";
        // Give the independently restarted process time to leave before cleanup.
        Sleep(500); std::error_code ec; fs::remove_all(root,ec); return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
