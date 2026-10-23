#pragma once

#include <optional>
#include <string>

namespace kemu {
class RotationEvents {
public:
    std::optional<int> accept(char ch) {
        if (ch != '\n') {
            if (mLine.size() < 128) mLine.push_back(ch);
            else mOverflow = true;
            return std::nullopt;
        }
        if (!mLine.empty() && mLine.back() == '\r') mLine.pop_back();
        std::optional<int> rotation;
        const std::string prefix = "KEMU_ROTATION_V1 ";
        if (!mOverflow && mLine.size() == prefix.size() + 1 &&
            mLine.compare(0, prefix.size(), prefix) == 0 &&
            mLine.back() >= '0' && mLine.back() <= '3') {
            const int value = mLine.back() - '0';
            if (mLast != value) rotation = value;
            mLast = value;
        }
        mLine.clear();
        mOverflow = false;
        return rotation;
    }

private:
    std::string mLine;
    bool mOverflow = false;
    int mLast = -1;
};
}  // namespace kemu
