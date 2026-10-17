#pragma once
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <string>

namespace kemu {
inline int hostCpuCount() {
    return (std::max)(1, int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
}
inline int hostRamGb() {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    return GlobalMemoryStatusEx(&memory) ? (std::max)(1, int(memory.ullTotalPhys / (1024ull * 1024 * 1024))) : 1;
}
struct PerformanceConfig {
    int width = 1920, height = 1080, fps = 60;
    int cores = (std::min)(8, hostCpuCount()), ramGb = (std::max)(4, (std::min)(16, hostRamGb()));
    int resolution = 1, fpsMode = 1, portrait = 0;
    int customWidth = 1920, customHeight = 1080, customFps = 60;
    std::wstring displayName;
    static constexpr int sizes[4][2]{{1280,720},{1920,1080},{2560,1440},{3840,2160}};
    void resolve() {
        if (resolution < 4) { width = sizes[resolution][0]; height = sizes[resolution][1]; }
        if ((portrait && width > height) || (!portrait && width < height)) std::swap(width,height);
        if (fpsMode < 2) fps = fpsMode == 0 ? 30 : 60;
        if (fpsMode == 4) fps = 120;
        if (fpsMode == 2) {
            DEVMODEW mode{}; mode.dmSize = sizeof(mode);
            const auto* device = displayName.empty() ? nullptr : displayName.c_str();
            bool found = EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &mode, 0) != FALSE;
            if (!found && device) found = EnumDisplaySettingsExW(nullptr, ENUM_CURRENT_SETTINGS, &mode, 0) != FALSE;
            if (found && mode.dmDisplayFrequency > 1) fps = std::clamp(int(mode.dmDisplayFrequency), 1, 1000);
        }
    }
    static PerformanceConfig load(const std::filesystem::path& path) {
        PerformanceConfig c;
        auto read = [&](const wchar_t* key, int fallback, int low, int high) {
            return std::clamp(int(GetPrivateProfileIntW(L"performance", key, fallback, path.c_str())), low, high);
        };
        c.width = read(L"width", c.width, 320, 7680);
        c.height = read(L"height", c.height, 320, 7680);
        c.fps = read(L"fps", c.fps, 1, 1000);
        c.cores = read(L"cores", c.cores, 1, hostCpuCount());
        c.ramGb = read(L"ramGb", c.ramGb, 4, (std::max)(4, hostRamGb()));
        c.resolution = read(L"resolution", c.resolution, 0, 4);
        c.fpsMode = read(L"fpsMode", c.fpsMode, 0, 4);
        c.portrait = read(L"portrait", c.portrait, 0, 1);
        c.customWidth = read(L"customWidth", c.width, 320, 7680);
        c.customHeight = read(L"customHeight", c.height, 320, 7680);
        c.customFps = read(L"customFps", c.fps, 1, 1000);
        wchar_t display[128]{};
        GetPrivateProfileStringW(L"performance", L"displayName", L"", display, 128, path.c_str());
        c.displayName = display;
        c.resolve();
        return c;
    }
};
}
