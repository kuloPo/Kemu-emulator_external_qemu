#pragma once

#include <QRegularExpression>
#include <QString>
#include <optional>

namespace kemu {
inline std::optional<int> displayRotation(const QString& response) {
    static const QRegularExpression viewport(
        "Viewport [A-Z]+: displayId=0,[^\\r\\n]*? orientation=([0-3]),");
    auto matches = viewport.globalMatch(response);
    std::optional<int> rotation;
    while (matches.hasNext()) {
        const int value = matches.next().captured(1).toInt();
        if (rotation && *rotation != value) return std::nullopt;
        rotation = value;
    }
    return rotation;
}
}  // namespace kemu
