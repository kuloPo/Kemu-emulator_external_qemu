#include "UpdateCore.h"
#include <windows.h>
#include <shellapi.h>
#include <string>

using namespace kemu::update;
static std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }
static void restart(const fs::path& root) {
    auto program = root / "Kemu.exe"; auto command = quote(program.wstring()); STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    require(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &si, &pi), "Update completed but Kemu could not restart");
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    fs::path diagnosticStage;
    try {
        int count = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &count); require(argv && count >= 3, "Usage: KemuUpdater --apply STAGE PARENT_PID | --recover ROOT");
        std::wstring mode = argv[1]; fs::path argument = argv[2]; DWORD pid = count == 4 ? std::stoul(argv[3]) : 0; LocalFree(argv);
        fs::path root, stage;
        if (mode == L"--apply") { require(pid != 0 && count == 4, "Missing parent process"); stage = argument; root = stage.parent_path(); }
        else { require(mode == L"--recover" && count == 3, "Invalid updater arguments"); root = argument; auto marker = readJson(root / ".kemu-update-pending"); require(marker.at("stage") == ".kemu-update", "Unexpected update directory"); stage = root / ".kemu-update"; }
        diagnosticStage = stage;
        std::wstring module(32768, L'\0'); module.resize(GetModuleFileNameW(nullptr, module.data(), DWORD(module.size())));
        require(fs::weakly_canonical(fs::path(module).parent_path()) == fs::weakly_canonical(root), "Updater must run from the installation root");
        HANDLE process = nullptr;
        if (mode == L"--apply") {
            process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            require(process, "Cannot open the parent emulator process");
            std::wstring parent(32768, L'\0'); DWORD length = DWORD(parent.size());
            bool valid = QueryFullProcessImageNameW(process, 0, parent.data(), &length); parent.resize(length);
            require(valid && fs::weakly_canonical(fs::path(parent)) == fs::weakly_canonical(root / "qemu/windows-x86_64/qemu-system-x86_64.exe"), "Unexpected parent emulator");
            validateJob(root, stage);
            auto marker = readJson(root / ".kemu-update-pending"); require(marker.at("stage") == stage.filename().u8string(), "Update marker mismatch");
            writeText(stage / "updater-ready", "ready\n");
            DWORD waited = WaitForSingleObject(process, 120000); CloseHandle(process);
            require(waited == WAIT_OBJECT_0, "Emulator did not exit; files were not changed");
        }
        HANDLE lock = CreateFileW((root / ".kemu-update-lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(lock != INVALID_HANDLE_VALUE, "Another updater is running");
        struct Lock { HANDLE h; ~Lock() { CloseHandle(h); } } release{lock};
        if (mode == L"--recover") recover(root, stage); else apply(root, stage);
        restart(root); return 0;
    } catch (const std::exception& error) {
        try { if (!diagnosticStage.empty() && fs::is_directory(diagnosticStage)) writeText(diagnosticStage / "error.txt", error.what()); } catch (...) {}
        auto detail = fs::u8path(error.what()).wstring();
        MessageBoxW(nullptr, (L"Kemu 更新未完成\n\n" + detail + L"\n\n已保留更新日志与备份；再次启动 Kemu 可恢复未完成的替换。").c_str(), L"Kemu 更新", MB_OK | MB_ICONERROR);
        return 1;
    }
}
