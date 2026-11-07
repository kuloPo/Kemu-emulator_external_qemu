#include "../update/UpdateCore.h"
#include <windows.h>

namespace kemu::update {
Installer fetchInstaller(const std::string& version) {
    std::wstring module(32768, L'\0');
    module.resize(GetModuleFileNameW(nullptr, module.data(), DWORD(module.size())));
    const auto root = fs::path(module).parent_path().parent_path();
    if (fs::exists(root / "fixture-network-error")) throw NetworkError("Fixture network failure");
    return selectInstaller(readJson(root / "fixture-release.json"), version);
}
}
