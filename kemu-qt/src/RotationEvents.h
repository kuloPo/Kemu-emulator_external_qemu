#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace kemu {
class RotationEvents {
public:
    // Rotation and logical size of the guest's default display.
    struct Display {
        int rotation = 0;
        int width = 0;
        int height = 0;
        bool operator==(const Display& other) const {
            return rotation == other.rotation && width == other.width && height == other.height;
        }
        bool operator!=(const Display& other) const { return !(*this == other); }
    };

    // What a completed line changed.
    struct Event {
        std::optional<int> rotation;
        std::optional<Display> display;
    };

    Event accept(char ch) {
        Event event;
        if (ch != '\n') {
            if (mLine.size() < 128) mLine.push_back(ch);
            else mOverflow = true;
            return event;
        }
        if (!mLine.empty() && mLine.back() == '\r') mLine.pop_back();
        const std::string prefix = "KEMU_ROTATION_V1 ";
        if (!mOverflow && mLine.size() == prefix.size() + 1 &&
            mLine.compare(0, prefix.size(), prefix) == 0 &&
            mLine.back() >= '0' && mLine.back() <= '3') {
            const int value = mLine.back() - '0';
            if (mLast != value) event.rotation = value;
            mLast = value;
        } else if (const auto display = mOverflow ? std::nullopt : parseDisplay(mLine)) {
            if (mLastDisplay != display) event.display = display;
            mLastDisplay = display;
        }
        mLine.clear();
        mOverflow = false;
        return event;
    }

private:
    // Parses "KEMU_DISPLAY_V1 <rotation> <width> <height>".
    static std::optional<Display> parseDisplay(std::string_view line) {
        const std::string_view prefix = "KEMU_DISPLAY_V1 ";
        if (line.substr(0, prefix.size()) != prefix) return std::nullopt;
        line.remove_prefix(prefix.size());
        int values[3] = {};
        for (int i = 0; i < 3; ++i) {
            if (i != 0) {
                if (line.empty() || line.front() != ' ') return std::nullopt;
                line.remove_prefix(1);
            }
            size_t digits = 0;
            while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') {
                if (++digits > 5) return std::nullopt;
                values[i] = values[i] * 10 + (line[digits - 1] - '0');
            }
            if (digits == 0 || (digits > 1 && line.front() == '0')) return std::nullopt;
            line.remove_prefix(digits);
        }
        if (!line.empty() || values[0] > 3 || values[1] == 0 || values[2] == 0) return std::nullopt;
        return Display{values[0], values[1], values[2]};
    }

    std::string mLine;
    bool mOverflow = false;
    int mLast = -1;
    std::optional<Display> mLastDisplay;
};
}  // namespace kemu
