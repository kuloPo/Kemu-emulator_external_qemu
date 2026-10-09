#include "RuntimeConfig.h"
#include <windows.h>
#include <fstream>
#include <stdexcept>

namespace kemu {
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
    if (fs::exists(applicationDir / "runtime/emulator/qemu/windows-x86_64/qemu-system-x86_64.exe")) {
        c.engine = applicationDir / "runtime/emulator";
        c.images = applicationDir / "images/emu64x";
        c.avdRoot = localAppData / "Kemu/Kemu/avd";
        c.logs = localAppData / "Kemu/Kemu/logs";
    } else {
        auto dir = applicationDir;
        for (int i = 0; i < 12; ++i) {
            if (fs::exists(dir / "guest-images/emu64x/system.img")) break;
            if (dir == dir.parent_path()) throw std::runtime_error("Cannot locate Kemu guest images");
            dir = dir.parent_path();
        }
        c.engine = dir / "host-build/out/emulator-37.2.10-vs2026/distribution/emulator";
        c.images = dir / "guest-images/emu64x";
        c.avdRoot = dir / "avd";
        c.logs = dir / "logs";
    }
    return c;
}

void RuntimeConfig::validate() const {
    for (const auto& file : {engine / "qemu/windows-x86_64/qemu-system-x86_64.exe",
                             engine / "kemu-native-ui-v1.txt"}) {
        if (!fs::is_regular_file(file)) throw std::runtime_error("Missing Kemu runtime: " + utf8(file.wstring()));
    }
    for (const auto* name : {"system.img", "vendor.img", "ramdisk.img", "kernel-ranchu", "userdata.img"}) {
        if (!fs::is_regular_file(images / name)) throw std::runtime_error("Missing Android image: " + utf8((images / name).wstring()));
    }
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
    return {L"-avd", L"kemu", L"-sysdir", images.wstring(), L"-port", L"5554",
            L"-cores", L"8", L"-memory", L"16384", L"-gpu", L"auto", L"-no-snapshot",
            L"-no-boot-anim", L"-no-metrics", L"-crash-report-mode", L"never"};
}
}  // namespace kemu
