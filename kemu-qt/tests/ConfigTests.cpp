#include "RuntimeConfig.h"
#include <windows.h>
#include <algorithm>
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

static void checkEngineDiscovery(const kemu::fs::path& temporary) {
    // Literal Unicode verifies paths that cannot be represented as ASCII.
    const auto project = temporary / L"工程";
    const auto build = project / "host-build/out/custom build";
    kemu::fs::create_directories(build);
    kemu::fs::create_directories(project / "guest-images/emu64x");
    std::ofstream(project / "guest-images/emu64x/system.img") << "image fixture";
    const auto development = kemu::RuntimeConfig::discover(build, temporary / "local");
    require(development.engine == build, "engine must use the launcher directory, regardless of build name");
    require(development.images == build / "system-img", "images must not fall back to the project directory");
    require(development.avdRoot == temporary / "local/Kemu/Kemu/avd", "AVD must stay in local app data");

    const auto release = project / "custom release";
    // Keep old layouts populated to detect accidental fallback when an image is missing.
    kemu::fs::create_directories(release / "images/emu64x");
    kemu::fs::create_directories(release / "system-img");
    for (const auto* name : {"system.img", "vendor.img", "ramdisk.img", "kernel-ranchu", "userdata.img"})
        std::ofstream(release / "images/emu64x" / name) << "image fixture";
    for (const auto* name : {"system.img", "ramdisk.img", "kernel-ranchu", "userdata.img"})
        std::ofstream(release / "system-img" / name) << "image fixture";
    // A leftover legacy runtime must not override the fixed relative path.
    const auto legacy = release / "runtime/emulator/qemu/windows-x86_64";
    kemu::fs::create_directories(legacy);
    std::ofstream(legacy / "qemu-system-x86_64.exe") << "legacy engine fixture";
    const auto installed = kemu::RuntimeConfig::discover(release, temporary / "local");
    require(installed.engine == release, "installed engine must use the launcher directory");
    require(installed.images == release / "system-img", "images must be in system-img beside the launcher");
    bool missingEngine = false;
    try { installed.validate(); } catch (const std::runtime_error& error) {
        missingEngine = std::string(error.what()).find(kemu::utf8(
            (release / "qemu/windows-x86_64/qemu-system-x86_64.exe").wstring())) != std::string::npos;
    }
    require(missingEngine, "missing adjacent engine must be reported instead of using a legacy runtime");
    kemu::fs::create_directories(release / "qemu/windows-x86_64");
    std::ofstream(release / "qemu/windows-x86_64/qemu-system-x86_64.exe") << "engine fixture";
    bool missingImage = false;
    try { installed.validate(); } catch (const std::runtime_error& error) {
        missingImage = std::string(error.what()).find(kemu::utf8(
            (release / "system-img" / "build.prop").wstring())) != std::string::npos;
    }
    require(missingImage, "missing image must be reported instead of using old image layouts");
    for (const auto* name : {"build.prop", "kernel_cmdline.txt", "VerifiedBootParams.textproto"})
        std::ofstream(installed.images / name) << "metadata fixture";
    std::ofstream(installed.images / "advancedFeatures.ini") << "EncryptUserData = off\nGuestAngle = off\n";
    // A complete runtime must validate without any external build-script marker.
    installed.validate();
    const auto arguments = installed.arguments();
    require(arguments.size() > 3 && arguments[2] == L"-sysdir" &&
            arguments[3] == (release / "system-img").wstring(), "engine must receive the system-img directory");
    require(arguments[arguments.size() - 2] == L"-feature" &&
            arguments.back() == L"GuestAngle,-EncryptUserData,-MultiDisplay", "single-display features must override the image defaults");
    const auto selinux = std::find(arguments.begin(), arguments.end(), L"-selinux");
    require(selinux != arguments.end() && selinux + 1 != arguments.end() &&
            *(selinux + 1) == L"permissive", "the emulator must start with permissive SELinux");

    for (const auto* name : {"build.prop", "advancedFeatures.ini", "kernel_cmdline.txt", "VerifiedBootParams.textproto"}) {
        const auto path = installed.images / name;
        const auto contents = read(path);
        kemu::fs::remove(path);
        for (bool directory : {false, true}) {
            if (directory) kemu::fs::create_directory(path);
            bool rejected = false;
            try { installed.validate(); } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()) == "Missing Android image: " + kemu::utf8(path.wstring());
            }
            require(rejected, "required metadata must be a file and report its full path");
        }
        kemu::fs::remove(path);
        std::ofstream(path, std::ios::binary) << contents;
    }
    for (const auto* setting : {"EncryptUserData=on\n", "# EncryptUserData=off\r\n  EncryptUserData = ON \r\n",
                               "\xEF\xBB\xBF" "EncryptUserData=on\n"}) {
        std::ofstream(installed.images / "advancedFeatures.ini") << setting;
        bool rejected = false;
        try { installed.validate(); } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()) == "Missing Android image: " +
                kemu::utf8((installed.images / "encryptionkey.img").wstring());
        }
        require(rejected, "encrypted image must require encryptionkey.img");
        std::ofstream(installed.images / "encryptionkey.img") << "key fixture";
        installed.validate();
        require(installed.arguments().back() == L"GuestAngle,EncryptUserData,-MultiDisplay", "encryption arguments must match validation");
        kemu::fs::remove(installed.images / "encryptionkey.img");
    }
    for (const auto* setting : {"EncryptUserData=off\n", "# EncryptUserData=on\n", "EncryptUserData=on\nEncryptUserData=off\n"}) {
        std::ofstream(installed.images / "advancedFeatures.ini") << setting;
        installed.validate();
        require(installed.arguments().back() == L"GuestAngle,-EncryptUserData,-MultiDisplay", "unencrypted images must not require a key");
    }
    std::ofstream(installed.images / "advancedFeatures.ini") << "EncryptUserData=invalid\n";
    bool invalidEncryption = false;
    try { installed.validate(); } catch (const std::runtime_error& error) {
        invalidEncryption = std::string(error.what()).find("Invalid EncryptUserData") != std::string::npos;
    }
    require(invalidEncryption, "invalid encryption setting must not silently disable key validation");
}

int main() {
    const auto temporary = kemu::fs::temp_directory_path() / (L"kemu-config-test-" + std::to_wstring(GetCurrentProcessId()));
    try {
        checkEngineDiscovery(temporary);
        kemu::RuntimeConfig config;
        // Exercise Unicode and spaces in both configuration and image paths.
        config.avdRoot = temporary / L"中文 path/avd";
        config.images = temporary / L"中文 images";
        config.logs = temporary / "logs";
        const auto avd = config.avdRoot / "kemu.avd";
        kemu::fs::create_directories(avd);
        const auto ini = avd / "config.ini";
        std::ofstream(ini) << "# user config\nhw.cpu.ncore=6\nhw.cpu.ncore=4\nhw.ramSize=8192\n"
                              "disk.dataPartition.size=128G\nhw.lcd.density=480\ncustom.option=keep-me\n";
        std::ofstream(avd / "userdata-qemu.img") << "existing applications";
        config.prepare();
        const auto first = read(ini);
        for (auto text : {"hw.lcd.width=1920\n",
                          "hw.lcd.height=1080\n", "hw.lcd.vsync=60\n", "hw.initialOrientation=landscape\n",
                          "disk.dataPartition.size=128G\n", "userdata.useQcow2=no\n",
                          "hw.lcd.density=480\n", "custom.option=keep-me\n"}) {
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
        const auto performanceFile = config.avdRoot / "performance.ini";
        std::ofstream(performanceFile) << "[performance]\nwidth=1440\nheight=2560\nfps=144\ncores=1\nramGb=1\nportrait=1\nresolution=2\nfpsMode=3\n";
        config.performance = kemu::PerformanceConfig::load(performanceFile);
        config.prepare();
        const auto changed = read(config.avdRoot / "kemu.avd/config.ini");
        for (const auto* value : {"hw.cpu.ncore=1\n", "hw.ramSize=4096\n", "hw.lcd.width=1440\n", "hw.lcd.height=2560\n", "hw.lcd.vsync=144\n", "hw.initialOrientation=portrait\n", "skin.name=1440x2560\n"})
            require(changed.find(value) != std::string::npos, "Performance configuration was not applied");
        kemu::fs::create_directories(config.images);
        std::ofstream(config.images / "advancedFeatures.ini") << "EncryptUserData=off\n";
        const auto args = config.arguments();
        require(args[7] == L"1" && args[9] == L"4096", "Performance launch arguments were not applied");
        std::ofstream(performanceFile) << "[performance]\ncores=999999\nramGb=999999\nwidth=0\nfps=0\nresolution=4\nfpsMode=3\nportrait=1\n";
        const auto limited = kemu::PerformanceConfig::load(performanceFile);
        require(limited.cores == kemu::hostCpuCount() && limited.ramGb == kemu::hostRamGb() && limited.width == 320 && limited.fps == 1, "Host limits were not enforced");
        std::ofstream(performanceFile) << "[performance]\nresolution=0\nwidth=999\nheight=999\nfpsMode=0\nfps=999\nramGb=1\nportrait=1\n";
        const auto preset = kemu::PerformanceConfig::load(performanceFile);
        require(preset.width==720 && preset.height==1280 && preset.fps==30 && preset.ramGb==4, "Preset settings must normalize before launch");
        std::ofstream(performanceFile) << "[performance]\nfpsMode=2\nfps=1\ndisplayName=missing-display\n";
        const auto screen = kemu::PerformanceConfig::load(performanceFile);
        DEVMODEW display{}; display.dmSize=sizeof(display);
        if (EnumDisplaySettingsExW(nullptr,ENUM_CURRENT_SETTINGS,&display,0) && display.dmDisplayFrequency>1)
            require(screen.fps==int(display.dmDisplayFrequency), "Screen FPS must refresh at launch and recover from a removed display");
        std::ofstream(performanceFile) << "[performance]\nfpsMode=4\nfps=30\n";
        require(kemu::PerformanceConfig::load(performanceFile).fps==120, "120 FPS preset must resolve before launch");
        const auto resolved = kemu::fs::canonical(temporary);
        require(resolved.parent_path() == kemu::fs::canonical(kemu::fs::temp_directory_path()) &&
                resolved.filename() == temporary.filename(), "unexpected cleanup path");
        kemu::fs::remove_all(resolved);
        std::cout << "PASS: launcher-relative paths, required image metadata, conditional encryption key, GuestAngle arguments, preserved userdata, settings, Unicode paths, idempotence, atomic replacement and argument quoting\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (fixtures retained at " << temporary << ")\n";
        return 1;
    }
}
