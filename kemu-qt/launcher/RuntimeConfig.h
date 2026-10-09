#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace kemu {
namespace fs = std::filesystem;
struct RuntimeConfig {
    fs::path engine, images, avdRoot, logs;
    static RuntimeConfig discover(const fs::path& applicationDir, const fs::path& localAppData);
    void validate() const;
    void prepare() const;
    std::vector<std::wstring> arguments() const;
};
std::string utf8(const std::wstring& value);
std::wstring quoteArgument(const std::wstring& value);
void updateIni(const fs::path& path, const std::map<std::string, std::string>& values);
}  // namespace kemu
