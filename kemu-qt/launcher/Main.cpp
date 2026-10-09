#include <winsock2.h>
#include <windows.h>
#include "RuntimeConfig.h"
#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace {
std::wstring environment(const wchar_t* key) {
    const DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    const DWORD used = GetEnvironmentVariableW(key, result.data(), size);
    result.resize(used);
    return result;
}
void setEnv(const wchar_t* key, const std::wstring& value) {
    if (!SetEnvironmentVariableW(key, value.c_str())) throw std::runtime_error("Cannot set runtime environment");
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring instanceName(const kemu::fs::path& avd) {
    auto name = kemu::fs::weakly_canonical(avd).wstring();
    std::transform(name.begin(), name.end(), name.begin(), std::towlower);
    uint64_t hash = 14695981039346656037ull;
    for (const auto c : name) { hash ^= uint16_t(c); hash *= 1099511628211ull; }
    return L"Kemu.Native." + std::to_wstring(hash);
}
bool activate(const std::wstring& name) {
    struct Search { const std::wstring* name; HWND found; } search{&name, nullptr};
    EnumWindows([](HWND window, LPARAM arg) -> BOOL {
        auto* state = reinterpret_cast<Search*>(arg);
        if (GetPropW(window, state->name->c_str())) { state->found = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (!search.found) return false;
    if (IsIconic(search.found)) ShowWindow(search.found, SW_RESTORE);
    SetForegroundWindow(search.found);
    return true;
}
void checkPorts() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data)) throw std::runtime_error("Cannot initialize network");
    bool available = true;
    for (int port : {5554, 5555}) {
        const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL exclusive = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(u_short(port));
        if (bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address))) available = false;
        closesocket(sock);
    }
    WSACleanup();
    if (!available) throw std::runtime_error("Emulator ports 5554/5555 are in use. Close the existing instance first.");
}
int launch() {
    std::wstring module(32768, L'\0');
    module.resize(GetModuleFileNameW(nullptr, module.data(), DWORD(module.size())));
    const auto config = kemu::RuntimeConfig::discover(kemu::fs::path(module).parent_path(), environment(L"LOCALAPPDATA"));
    const auto instance = instanceName(config.avdRoot);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle lease{CreateMutexW(&security, FALSE, (L"Local\\" + instance).c_str())};
    const DWORD mutexStatus = GetLastError();
    if (!lease.value) throw std::runtime_error("Cannot create instance lock");
    if (mutexStatus == ERROR_ALREADY_EXISTS) {
        for (int i = 0; i < 50 && !activate(instance); ++i) Sleep(100);
        return 0;
    }
    config.validate();
    checkPorts();
    config.prepare();
    setEnv(L"ANDROID_AVD_HOME", config.avdRoot.wstring());
    setEnv(L"ANDROID_EMULATOR_HOME", (config.avdRoot / "emulator-home").wstring());
    setEnv(L"ANDROID_EMULATOR_LAUNCHER_DIR", config.engine.wstring());
    setEnv(L"KEMU_UI", L"1");
    setEnv(L"KEMU_INSTANCE", instance);
    setEnv(L"KEMU_INSTANCE_HANDLE", std::to_wstring(reinterpret_cast<uintptr_t>(lease.value)));
    SetEnvironmentVariableW(L"QT_SCALE_FACTOR", nullptr);
    SetEnvironmentVariableW(L"QML2_IMPORT_PATH", nullptr);
    setEnv(L"QT_PLUGIN_PATH", (config.engine / "lib64/qt/plugins").wstring());
    setEnv(L"QT_QPA_PLATFORM_PLUGIN_PATH", (config.engine / "lib64/qt/plugins").wstring());
    std::wstring path;
    for (const auto* sub : {L"lib64/qt/lib", L"lib64", L"lib64/gles_swiftshader", L"lib64/vulkan", L""}) {
        path += (config.engine / sub).wstring() + L';';
    }
    setEnv(L"PATH", path + environment(L"PATH"));
    // Optional platform-tools can be added manually when publishing. The UI
    // checks availability once and warns without preventing the VM from booting.
    const auto bundledRoot = kemu::fs::path(module).parent_path() / "runtime";
    std::vector<kemu::fs::path> sdkRoots;
    if (kemu::fs::is_regular_file(bundledRoot / "platform-tools/adb.exe")) sdkRoots.push_back(bundledRoot);
    for (const auto* key : {L"ANDROID_HOME", L"ANDROID_SDK_ROOT"}) {
        auto value = environment(key);
        if (value.size() > 1 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
        if (!value.empty()) sdkRoots.emplace_back(value);
    }
    sdkRoots.push_back(kemu::fs::path(environment(L"LOCALAPPDATA")) / "Android/Sdk");
    kemu::fs::path sdk;
    for (const auto& candidate : sdkRoots) {
        std::error_code error;
        if (kemu::fs::is_directory(candidate / "platform-tools", error)) { sdk = candidate; break; }
    }
    if (sdk.empty()) {
        // Upstream AVD loading requires this directory even with explicit
        // image paths. A directory-only SDK lets the GUI warn about missing
        // ADB instead of exiting before a window can be shown.
        sdk = config.avdRoot / "emulator-home/sdk";
        kemu::fs::create_directories(sdk / "platform-tools");
    }
    setEnv(L"ANDROID_HOME", sdk.wstring());
    setEnv(L"ANDROID_SDK_ROOT", sdk.wstring());
    const auto engine = config.engine / "qemu/windows-x86_64/qemu-system-x86_64.exe";
    auto command = kemu::quoteArgument(engine.wstring());
    for (const auto& arg : config.arguments()) command += L' ' + kemu::quoteArgument(arg);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t logName[96]{};
    swprintf_s(logName, L"kemu-%04u%02u%02u-%02u%02u%02u-%lu.log", now.wYear, now.wMonth,
               now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    Handle log{CreateFileW((config.logs / logName).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (log.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open runtime log");
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) throw std::runtime_error("Cannot initialize process attributes");
    HANDLE inherited[]{lease.value, log.value, input.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(list);
        throw std::runtime_error("Cannot pass instance lease to emulator");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log.value;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(engine.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, config.engine.c_str(), &startup.StartupInfo, &process);
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(list);
    if (!started) throw std::runtime_error("Cannot start emulator (Windows " + std::to_string(error) + ")");
    Handle child{process.hProcess}, thread{process.hThread};
    // Catch loader/argument errors before relinquishing the bootstrap process.
    if (WaitForSingleObject(child.value, 1200) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess(child.value, &exitCode);
        if (exitCode) throw std::runtime_error("Emulator exited (" + std::to_string(exitCode) + "). Log: " + kemu::utf8((config.logs / logName).wstring()));
    }
    return 0;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try { return launch(); }
    catch (const std::exception& error) {
        const std::string text = error.what();
        const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
        std::wstring detail(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), detail.data(), size);
        MessageBoxW(nullptr, (L"暂时无法启动 Kemu\n\n" + detail).c_str(), L"Kemu", MB_OK | MB_ICONERROR);
        return 1;
    }
}
