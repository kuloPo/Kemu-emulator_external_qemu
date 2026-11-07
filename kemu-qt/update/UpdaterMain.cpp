#include "UpdateCore.h"
#include <windows.h>
#include <shellapi.h>
#include <memory>
#include <string>

using namespace kemu::update;
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }
void restart(const fs::path& root, bool waitForHelper = false) {
    const auto program = root / "Kemu.exe";
    auto command = quote(program.wstring());
    if (waitForHelper) command += L" --restart-pid " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    require(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &si, &pi), "Kemu could not restart");
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    fs::path diagnosticStage, root;
    bool fullInstaller = false, parentExited = false, cancelled = false;
    try {
        int count = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
        require(argv && count >= 3, "Usage: KemuUpdater --apply/--install STAGE PARENT_PID | --recover ROOT");
        std::wstring mode = argv[1]; fs::path argument = argv[2]; DWORD pid = count == 4 ? std::stoul(argv[3]) : 0; LocalFree(argv);
        fs::path stage;
        fullInstaller = mode == L"--install";
        if (mode == L"--apply" || fullInstaller) { require(pid != 0 && count == 4, "Missing parent process"); stage = argument; root = stage.parent_path(); }
        else { require(mode == L"--recover" && count == 3, "Invalid updater arguments"); root = argument; auto marker = readJson(root / ".kemu-update-pending"); require(marker.at("stage") == ".kemu-update", "Unexpected update directory"); stage = root / ".kemu-update"; }
        diagnosticStage = stage;
        std::wstring module(32768, L'\0'); module.resize(GetModuleFileNameW(nullptr, module.data(), DWORD(module.size())));
        require(stage.filename() == ".kemu-update", "Unexpected update directory");
        require(fs::weakly_canonical(fs::path(module)) == fs::weakly_canonical(fullInstaller ? stage / "KemuInstallerLauncher.exe" : root / "KemuUpdater.exe"), "Unexpected updater location");
        Handle lock{CreateFileW((root / ".kemu-update-lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(lock.value != INVALID_HANDLE_VALUE, "Another updater is running");
        Installer installer;
        std::unique_ptr<LockedInstaller> lockedInstaller;
        if (mode == L"--apply" || fullInstaller) {
            Handle process{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
            require(process.value, "Cannot open the parent emulator process");
            std::wstring parent(32768, L'\0'); DWORD length = DWORD(parent.size());
            bool valid = QueryFullProcessImageNameW(process.value, 0, parent.data(), &length); parent.resize(length);
            require(valid && fs::weakly_canonical(fs::path(parent)) == fs::weakly_canonical(root / "qemu/windows-x86_64/qemu-system-x86_64.exe"), "Unexpected parent emulator");
            if (fullInstaller) {
                fs::path cached; Plan plan;
                require(!fs::exists(root / ".kemu-update-pending") &&
                        loadDownloadedUpdate(root, cached, plan, FullInstallerCheck::MetadataOnly) && plan.requiresFullInstaller,
                        "The full installer is not ready");
                installer = fetchInstaller(plan.target);
                lockedInstaller = std::make_unique<LockedInstaller>(stage, installer);
            } else {
                validateJob(root, stage);
                auto marker = readJson(root / ".kemu-update-pending"); require(marker.at("stage") == stage.filename().u8string(), "Update marker mismatch");
            }
            writeText(stage / (fullInstaller ? "installer-ready" : "updater-ready"), "ready\n");
            require(WaitForSingleObject(process.value, 120000) == WAIT_OBJECT_0, "Emulator did not exit; files were not changed");
            parentExited = true;
        }
        if (fullInstaller) {
            const auto program = stage / installer.name;
            const auto parameters = L"/DIR=" + quote(root.wstring()) + L" /NOFORCECLOSEAPPLICATIONS /NORESTART";
            SHELLEXECUTEINFOW execute{sizeof(execute)};
            execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            execute.lpVerb = L"open"; execute.lpFile = program.c_str(); execute.lpParameters = parameters.c_str();
            execute.lpDirectory = root.c_str(); execute.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&execute)) {
                const auto error = GetLastError();
                cancelled = error == ERROR_CANCELLED;
                throw std::runtime_error("Cannot open the full installer (Windows " + std::to_string(error) + ")");
            }
            Handle process{execute.hProcess};
            require(process.value && WaitForSingleObject(process.value, INFINITE) == WAIT_OBJECT_0, "Cannot wait for the full installer");
            DWORD result = 1;
            require(GetExitCodeProcess(process.value, &result), "Cannot read the installer result");
            lockedInstaller.reset();
            if (result == 0) {
                try { cleanupInstalledFullUpdate(root, true); }
                catch (const std::exception& error) {
                    try { writeText(stage / "error.txt", "Update cache cleanup: " + std::string(error.what())); } catch (...) {}
                }
            }
            return 0;
        }
        if (mode == L"--recover") recover(root, stage); else apply(root, stage);
        restart(root); return 0;
    } catch (const std::exception& error) {
        try {
            if (!diagnosticStage.empty() && fs::is_directory(diagnosticStage)) {
                writeText(diagnosticStage / "error.txt", error.what());
                if (fullInstaller && !parentExited) writeJson(diagnosticStage / "installer-error.json",
                    {{"pid",GetCurrentProcessId()},{"message",error.what()}});
            }
        } catch (...) {}
        const auto detail = fs::u8path(error.what()).wstring();
        if (fullInstaller) {
            if (!parentExited) return 1;
            if (!cancelled) MessageBoxW(nullptr, (L"The full update could not finish.\n\n" + detail +
                L"\n\nKemu will reopen after this message is closed.").c_str(), L"Kemu Update", MB_OK | MB_ICONERROR);
            try { restart(root, true); }
            catch (const std::exception& restartError) {
                MessageBoxW(nullptr, fs::u8path(restartError.what()).c_str(), L"Kemu Update", MB_OK | MB_ICONERROR);
            }
            return cancelled ? 0 : 1;
        }
        MessageBoxW(nullptr, (L"Kemu 更新未完成\n\n" + detail + L"\n\n已保留更新日志与备份；再次启动 Kemu 可恢复未完成的替换。").c_str(), L"Kemu 更新", MB_OK | MB_ICONERROR);
        return 1;
    }
}
