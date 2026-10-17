#pragma once
#include <QSize>

namespace kemu {
struct WindowSizing {
    QSize initial;
    QSize minimum;
};
inline WindowSizing windowSizing(QSize display, const QSize& screen) {
    if (display.isEmpty()) display = QSize(1920,1080);
    const QSize chrome(54,42);
    const QSize bounds(qMax(1,qMin(1280,screen.width()-112)),
                       qMax(1,qMin(1280,screen.height()-106)));
    const QSize viewport = display.scaled(bounds,Qt::KeepAspectRatio).expandedTo(QSize(1,1));
    QSize minimum = display.scaled(QSize(320,452),Qt::KeepAspectRatioByExpanding);
    if (minimum.width()>viewport.width() || minimum.height()>viewport.height()) minimum=viewport;
    return {viewport+chrome,minimum+chrome};
}
}
