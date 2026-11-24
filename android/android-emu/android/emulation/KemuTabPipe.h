#pragma once

#include <optional>
#include <atomic>
#include <string>
#include "render-utils/display_operations.h"

namespace kemu {
bool appTabsEnabled();
extern std::atomic<int> presentedTab;
bool setPresentationView(const gfxstream_display_view& view);
bool getPresentationView(gfxstream_display_view* view);
// The UI consumes full guest snapshots. An empty snapshot marks disconnect.
std::optional<std::string> takeTabSnapshot();
std::string tabIconPng(const std::string& token);
void sendTabCommand(const std::string& message);
}
