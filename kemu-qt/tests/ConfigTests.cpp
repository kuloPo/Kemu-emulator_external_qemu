#include "RuntimeConfig.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

static std::string read(const kemu::fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    const auto temporary = kemu::fs::temp_directory_path() / (L"kemu-config-test-" + std::to_wstring(GetCurrentProcessId()));
    try {
        kemu::RuntimeConfig config;
        config.avdRoot = temporary / L"含 空格/avd";
        config.images = temporary / L"新镜像 images";
        config.logs = temporary / "logs";
        const auto avd = config.avdRoot / "kemu.avd";
        kemu::fs::create_directories(avd);
        const auto ini = avd / "config.ini";
        std::ofstream(ini) << "# user config\nhw.cpu.ncore=6\nhw.cpu.ncore=4\nhw.ramSize=8192\n"
                              "disk.dataPartition.size=128G\nhw.lcd.density=480\ncustom.option=keep-me\n";
        std::ofstream(avd / "userdata-qemu.img") << "existing applications";
        config.prepare();
        const auto first = read(ini);
        for (auto text : {"hw.cpu.ncore=8\n", "hw.ramSize=16384\n", "hw.lcd.width=1920\n",
                          "hw.lcd.height=1080\n", "hw.lcd.vsync=60\n", "hw.initialOrientation=landscape\n",
                          "disk.dataPartition.size=128G\n", "hw.lcd.density=480\n", "custom.option=keep-me\n"}) {
            require(first.find(text) != std::string::npos, text);
        }
        require(first.find("hw.cpu.ncore=", first.find("hw.cpu.ncore=") + 1) == std::string::npos, "duplicate CPU setting");
        config.prepare();
        require(read(ini) == first, "prepare must be idempotent");
        require(read(avd / "userdata-qemu.img") == "existing applications", "userdata changed");
        config.images = temporary / "moved images";
        config.prepare();
        require(read(ini).find("moved images") != std::string::npos, "image path not updated");
        bool rejected = false;
        try { kemu::updateIni(avd, {{"key","value"}}); } catch (...) { rejected = true; }
        require(rejected && kemu::fs::is_directory(avd), "directory was overwritten");
        config.avdRoot = temporary / "fresh";
        config.prepare();
        require(read(config.avdRoot / "kemu.avd/config.ini").find("disk.dataPartition.size=32G") != std::string::npos, "new instance default missing");
        require(kemu::quoteArgument(L"C:\\space path\\") == L"\"C:\\space path\\\\\"", "trailing slash quoting");
        require(kemu::quoteArgument(L"a\"b") == L"\"a\\\"b\"", "embedded quote escaping");
        const auto resolved = kemu::fs::canonical(temporary);
        require(resolved.parent_path() == kemu::fs::canonical(kemu::fs::temp_directory_path()) &&
                resolved.filename() == temporary.filename(), "unexpected cleanup path");
        kemu::fs::remove_all(resolved);
        std::cout << "PASS: preserved userdata, settings, Unicode paths, idempotence, atomic replacement and argument quoting\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (fixtures retained at " << temporary << ")\n";
        return 1;
    }
}
