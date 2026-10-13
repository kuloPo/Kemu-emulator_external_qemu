#include "RuntimeConfig.h"
#include <windows.h>
#include <fstream>
#include <stdexcept>

namespace kemu {
namespace {
std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() :
        value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

bool imageEncryptionEnabled(const fs::path& images) {
    const auto path = images / "advancedFeatures.ini";
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read configuration: " + utf8(path.wstring()));
    bool enabled = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        const auto equal = line.find('=');
        if (equal == std::string::npos || trim(line.substr(0, equal)) != "EncryptUserData") continue;
        const auto value = trim(line.substr(equal + 1));
        if (_stricmp(value.c_str(), "on") == 0) enabled = true;
        else if (_stricmp(value.c_str(), "off") == 0) enabled = false;
        else throw std::runtime_error("Invalid EncryptUserData (expected on or off): " + utf8(path.wstring()));
    }
    if (in.bad()) throw std::runtime_error("Cannot read configuration: " + utf8(path.wstring()));
    return enabled;
}
}  // namespace

std::string utf8(const std::wstring& value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

RuntimeConfig RuntimeConfig::discover(const fs::path& applicationDir, const fs::path& localAppData) {
    RuntimeConfig c;
    // The engine and its libraries are installed alongside Kemu.exe.
    c.engine = applicationDir;
    c.images = applicationDir / "system-img";
    c.avdRoot = localAppData / "Kemu/Kemu/avd";
    c.logs = localAppData / "Kemu/Kemu/logs";
    return c;
}

void RuntimeConfig::validate() const {
    const auto executable = engine / "qemu/windows-x86_64/qemu-system-x86_64.exe";
    if (!fs::is_regular_file(executable)) throw std::runtime_error("Missing Kemu runtime: " + utf8(executable.wstring()));
    // This launcher uses the emu64xa image profile, including its verified boot metadata.
    for (const auto* name : {"system.img", "vendor.img", "ramdisk.img", "kernel-ranchu", "userdata.img",
                            "build.prop", "advancedFeatures.ini", "kernel_cmdline.txt", "VerifiedBootParams.textproto"}) {
        if (!fs::is_regular_file(images / name)) throw std::runtime_error("Missing Android image: " + utf8((images / name).wstring()));
    }
    const auto key = images / "encryptionkey.img";
    if (imageEncryptionEnabled(images) && !fs::is_regular_file(key))
        throw std::runtime_error("Missing Android image: " + utf8(key.wstring()));
}

void updateIni(const fs::path& path, const std::map<std::string, std::string>& values) {
    std::string output;
    if (fs::exists(path)) {
        if (!fs::is_regular_file(path)) throw std::runtime_error("Configuration path is not a file");
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read configuration: " + utf8(path.wstring()));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto equal = line.find('=');
            auto key = line.substr(0, equal);
            const auto first = key.find_first_not_of(" \t");
            if (first != std::string::npos) key = key.substr(first, key.find_last_not_of(" \t") - first + 1);
            if (equal != std::string::npos && values.count(key)) continue;
            if (!line.empty()) output += line + '\n';
        }
        if (in.bad()) throw std::runtime_error("Cannot read configuration");
    }
    for (const auto& [key, value] : values) output += key + '=' + value + '\n';
    auto temporary = path;
    temporary += L".kemu-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(output.data(), std::streamsize(output.size()));
        out.close();
        if (!out) throw std::runtime_error("Cannot write configuration: " + utf8(temporary.wstring()));
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Cannot replace configuration (Windows " + std::to_string(error) + ")");
    }
}

void RuntimeConfig::prepare() const {
    const auto avd = avdRoot / "kemu.avd";
    fs::create_directories(avd);
    fs::create_directories(logs);
    fs::create_directories(avdRoot / "emulator-home");
    const auto ini = avd / "config.ini";
    std::map<std::string, std::string> values;
    if (!fs::exists(ini)) {
        values = {{"AvdId","kemu"}, {"avd.ini.displayname","Kemu"}, {"avd.ini.encoding","UTF-8"},
            {"PlayStore.enabled","false"}, {"abi.type","x86_64"}, {"hw.cpu.arch","x86_64"},
            {"disk.dataPartition.size","32G"}, {"hw.lcd.density","320"}, {"hw.gpu.enabled","yes"},
            {"hw.gpu.mode","auto"}, {"hw.keyboard","yes"}, {"hw.audioOutput","yes"},
            {"tag.id","default"}, {"tag.display","Default Android System"}};
    }
    for (const auto& [key, value] : std::map<std::string, std::string>{
            {"hw.cpu.ncore","8"}, {"hw.ramSize","16384"}, {"hw.lcd.width","1920"},
            {"hw.lcd.height","1080"}, {"hw.lcd.vsync","60"}, {"hw.initialOrientation","landscape"},
            {"fastboot.forceColdBoot","yes"}, {"skin.dynamic","no"}, {"skin.name","1920x1080"},
            {"skin.path","1920x1080"}, {"image.sysdir.1",utf8(images.wstring())}}) values[key] = value;
    updateIni(ini, values);
    updateIni(avdRoot / "kemu.ini", {{"avd.ini.encoding","UTF-8"}, {"path",utf8(avd.wstring())},
                                     {"path.rel","avd/kemu.avd"}, {"target","android-36"}});
}

std::vector<std::wstring> RuntimeConfig::arguments() const {
    // Pin the image's encryption setting so user/environment feature overrides cannot
    // enable encryption after validation skipped its key, or disable an encrypted image.
    const auto features = imageEncryptionEnabled(images) ?
        L"GuestAngle,EncryptUserData" : L"GuestAngle,-EncryptUserData";
    return {L"-avd", L"kemu", L"-sysdir", images.wstring(), L"-port", L"5554",
            L"-cores", L"8", L"-memory", L"16384", L"-gpu", L"auto", L"-no-snapshot",
            L"-no-boot-anim", L"-no-metrics", L"-crash-report-mode", L"never",
            L"-feature", features};
}
}  // namespace kemu
