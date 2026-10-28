#include "RuntimeConfig.h"
#include "StorageConfig.h"
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

static void checkStorageGrowth(const kemu::fs::path& temporary) {
    namespace disk = kemu::storage;
    const auto avd = temporary / "storage";
    kemu::fs::create_directories(avd / "kemu.avd");
    const auto image = disk::imagePath(avd);
    const std::string contents(1024 * 1024, 'A');
    { std::ofstream out(image,std::ios::binary); out << contents; }
    disk::requestGrowth(avd,2);
    require(kemu::fs::file_size(image)==contents.size(),"Request changed a running disk");
    {
        disk::Handle reader{CreateFileW(image.c_str(),GENERIC_READ,FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr,OPEN_EXISTING,0,nullptr)};
        require(reader.value!=INVALID_HANDLE_VALUE,"Cannot lock storage fixture");
        bool locked=false;
        try { disk::applyGrowth(avd); } catch (const std::exception&) { locked=true; }
        require(locked && kemu::fs::file_size(image)==contents.size(),"Growth must require exclusive access");
    }
    require(disk::applyGrowth(avd)==2 && disk::applyGrowth(avd)==2,"Expansion recovery is not idempotent");
    const auto used=disk::usage(avd);
    require(used.capacity==2*disk::GiB && used.allocated<2*contents.size(),"Expansion allocated virtual capacity");
    {
        std::ifstream in(image,std::ios::binary);
        std::string prefix(contents.size(),'\0'); in.read(prefix.data(),prefix.size());
        require(prefix==contents,"Expansion changed existing data");
        in.seekg(used.capacity-1); char tail=1; in.read(&tail,1);
        require(in && tail==0,"New sparse tail must read as zero");
    }
    require(disk::readRequest(avd).target==used.capacity,"Request disappeared before guest completion");
    disk::completeGrowth(avd,disk::readRequest(avd));
    require(!disk::readRequest(avd).target,"Completed expansion remains pending");
    for (const uint64_t invalid : {uint64_t{0},uint64_t{1},uint64_t{2},disk::MaxGiB+1}) {
        bool rejected=false;
        try { disk::requestGrowth(avd,invalid); } catch (const std::exception&) { rejected=true; }
        require(rejected && !kemu::fs::exists(disk::requestPath(avd)),"Invalid capacity was accepted");
    }
    disk::requestGrowth(avd,3);
    const auto overlay=kemu::fs::path(image.wstring()+L".qcow2");
    { std::ofstream out(overlay); out << "overlay"; }
    bool rejected=false;
    try { disk::applyGrowth(avd); } catch (const std::exception&) { rejected=true; }
    require(rejected && kemu::fs::file_size(image)==used.capacity,"QCOW2 overlay was ignored");
    kemu::fs::remove(overlay);
    { std::ofstream out(disk::requestPath(avd)); out << "1 4096 " << 3*disk::GiB << '\n'; }
    rejected=false;
    try { disk::applyGrowth(avd); } catch (const std::exception&) { rejected=true; }
    require(rejected && kemu::fs::file_size(image)==used.capacity,"Stale request changed the disk");
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

static void checkStorageMove(const kemu::fs::path& temporary) {
    namespace disk = kemu::storage;
    const auto avd = temporary / "move/avd";
    const auto destination = temporary / L"move/磁盘 space";
    kemu::fs::create_directories(avd / "kemu.avd");
    kemu::fs::create_directories(destination);
    const auto original = disk::imagePath(avd);
    { std::ofstream out(original,std::ios::binary); out << std::string(4096,'A'); }
    disk::requestGrowth(avd,128); disk::applyGrowth(avd);
    bool rejected = false;
    try { disk::requestMove(avd,destination); } catch (...) { rejected = true; }
    require(rejected,"Moving must wait for guest expansion to finish");
    disk::completeGrowth(avd,disk::readRequest(avd));
    {
        std::fstream file(original,std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(127 * disk::GiB); file << "tail data";
    }
    for (const auto& directory : {original.parent_path(),temporary / "absent"}) {
        rejected = false;
        try { disk::requestMove(avd,directory); } catch (...) { rejected = true; }
        require(rejected && !kemu::fs::exists(disk::moveRequestPath(avd)),"Invalid move destination was accepted");
    }
    const auto target = destination / "userdata-qemu.img";
    { std::ofstream out(target); out << "existing disk"; }
    rejected = false;
    try { disk::requestMove(avd,destination); } catch (...) { rejected = true; }
    require(rejected && read(target)=="existing disk","Move overwrote a destination disk");
    kemu::fs::remove(target);
    disk::requestMove(avd,destination);
    require(disk::imagePath(avd)==original && kemu::fs::exists(original),"Request changed a running disk");
    {
        disk::Handle lock{CreateFileW(original.c_str(),GENERIC_READ,FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr,OPEN_EXISTING,0,nullptr)};
        rejected = false;
        try { disk::applyMove(avd); } catch (...) { rejected = true; }
        require(rejected && disk::imagePath(avd)==original,"Move must require exclusive disk access");
    }
    disk::requestMove(avd,destination);
    rejected = false;
    try { disk::requestGrowth(avd,129); } catch (...) { rejected = true; }
    require(rejected,"Expansion must not race a pending move");
    rejected = false;
    try { disk::applyMove(avd,[](int) { throw std::runtime_error("Simulated copy failure"); }); }
    catch (...) { rejected = true; }
    require(rejected && disk::imagePath(avd)==original && kemu::fs::is_empty(destination),
            "Copy failure must preserve the original and remove the partial copy");
    disk::requestMove(avd,destination);
    int progress = -1;
    require(disk::applyMove(avd,[&](int value) {
        require(value>=progress && value<=100,"Invalid move progress"); progress=value;
    }),"Pending move was ignored");
    require(progress==100 && !kemu::fs::exists(original) && kemu::fs::equivalent(disk::imagePath(avd),target),
            "Move did not switch to the verified copy");
    require(!disk::applyMove(avd),"Completed move must not repeat");
    const auto used = disk::usage(avd);
    require(used.capacity==128*disk::GiB && used.allocated<1024*1024,"Move lost sparse allocation or capacity");
    {
        std::ifstream in(target,std::ios::binary);
        std::string prefix(4096,'\0'); in.read(prefix.data(),prefix.size());
        require(prefix==std::string(4096,'A'),"Move changed the disk header");
        in.seekg(64 * disk::GiB); char hole=1; in.read(&hole,1);
        require(in && hole==0,"Move changed a sparse hole");
        in.seekg(127 * disk::GiB); std::string tail(9,'\0'); in.read(tail.data(),tail.size());
        require(tail=="tail data","Move lost data near the end of the disk");
    }
    disk::requestGrowth(avd,129); disk::applyGrowth(avd);
    require(kemu::fs::file_size(target)==129*disk::GiB,"Expansion did not use the moved disk");
    disk::completeGrowth(avd,disk::readRequest(avd));
    kemu::RuntimeConfig config;
    config.avdRoot=avd; config.images=temporary / "move/images";
    kemu::fs::create_directories(config.images);
    std::ofstream(config.images / "advancedFeatures.ini") << "EncryptUserData=off\n";
    const auto args=config.arguments();
    const auto data=std::find(args.begin(),args.end(),L"-data");
    require(data!=args.end() && kemu::fs::equivalent(*(data+1),target),"Launcher did not select the moved disk");
    kemu::fs::remove(target);
    rejected=false;
    try { config.validate(); } catch (const std::exception& error) {
        rejected=std::string(error.what()).find("configured data image is unavailable")!=std::string::npos;
    }
    require(rejected && !kemu::fs::exists(original),"Unavailable external disk must not reset user data");
}

int main(int argc, char** argv) {
    const auto temporary = kemu::fs::temp_directory_path() / (L"kemu-config-test-" + std::to_wstring(GetCurrentProcessId()));
    try {
        if (argc == 3 && std::string(argv[1]) == "--apply-growth") {
            const auto avd = kemu::fs::u8path(argv[2]);
            const auto capacity = kemu::storage::applyGrowth(avd);
            require(capacity != 0,"No expansion request for the integration fixture");
            kemu::updateIni(avd / "kemu.avd/config.ini",{{"disk.dataPartition.size",std::to_string(capacity)+"G"}});
            std::cout << "Prepared " << capacity << " GiB integration fixture\n";
            return 0;
        }
        checkEngineDiscovery(temporary);
        checkStorageGrowth(temporary);
        checkStorageMove(temporary);
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
        require(read(config.avdRoot / "kemu.avd/config.ini").find("disk.dataPartition.size=128G\n") != std::string::npos, "new instance default missing");
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
