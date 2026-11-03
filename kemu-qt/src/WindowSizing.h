#pragma once
#include <QRect>
#include <QRegion>
#include <QSize>
#include <QSizeF>
#include <cmath>

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

inline QSize fitWindowSize(const QSizeF& requested, const QSize& display,
                           const QSize& chrome, const QSize& minimum,
                           const QSize& maximum) {
    if (display.isEmpty()) return requested.toSize().boundedTo(maximum);
    if (maximum.width() <= chrome.width() || maximum.height() <= chrome.height()) return maximum;
    const QSize rounded = requested.toSize();
    const QSize viewport = rounded - chrome;
    // Preserve already rounded sizes across repeated rotation and restore cycles.
    if (rounded.width() >= minimum.width() && rounded.height() >= minimum.height() &&
        rounded.width() <= maximum.width() && rounded.height() <= maximum.height() &&
        !viewport.isEmpty() &&
        qAbs(qint64(viewport.width()) * display.height() - qint64(viewport.height()) * display.width()) <=
            qMax(display.width(), display.height()) / 2) return rounded;
    const double maximumScale = qMin(double(maximum.width() - chrome.width()) / display.width(),
                                     double(maximum.height() - chrome.height()) / display.height());
    const double minimumScale = qMax(double(qMax(1, minimum.width() - chrome.width())) / display.width(),
                                     double(qMax(1, minimum.height() - chrome.height())) / display.height());
    const double requestedScale = qMin((requested.width() - chrome.width()) / display.width(),
                                       (requested.height() - chrome.height()) / display.height());
    const double scale = qBound(qMin(minimumScale, maximumScale), requestedScale, maximumScale);
    if (display.width() >= display.height()) {
        const int width = qMax(1, qRound(display.width() * scale));
        return QSize(width, qMax(1, qRound(double(width) * display.height() / display.width()))) + chrome;
    }
    const int height = qMax(1, qRound(display.height() * scale));
    return QSize(qMax(1, qRound(double(height) * display.width() / display.height())), height) + chrome;
}

// Rectangles and the drag-start size are native pixels; chrome and minimum are logical sizes.
inline QRect constrainWindowResize(const QRect& proposed, Qt::Edges edges,
                                   const QSize& initial, const QSize& display,
                                   const QSize& chrome, const QSize& minimum,
                                   const QRegion& workArea, qreal dpr = 1.0,
                                   const QSize& maximumTrack = {}) {
    if (display.isEmpty() || !edges || workArea.isEmpty() || dpr <= 0) return proposed;
    const bool horizontal = edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
    const bool vertical = edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);
    const bool useWidth = horizontal && (!vertical ||
        qAbs(double(proposed.width() - initial.width())) / display.width() >=
        qAbs(double(proposed.height() - initial.height())) / display.height());
    const double scale = useWidth ? (proposed.width() / dpr - chrome.width()) / display.width()
                                  : (proposed.height() / dpr - chrome.height()) / display.height();
    const auto anchored = [&](const QSize& size) {
        return QRect(QPoint(edges.testFlag(Qt::LeftEdge) ? proposed.right() + 1 - size.width() : proposed.left(),
                            edges.testFlag(Qt::TopEdge) ? proposed.bottom() + 1 - size.height() : proposed.top()), size);
    };
    // Existing off-screen pixels remain allowed; only additional overflow is limited.
    const QRegion allowed = workArea.united(anchored(initial));
    const QRect bounds = allowed.boundingRect();
    QSize nativeMaximum(
        edges.testFlag(Qt::LeftEdge) ? proposed.right() + 1 - bounds.left() : bounds.right() + 1 - proposed.left(),
        edges.testFlag(Qt::TopEdge) ? proposed.bottom() + 1 - bounds.top() : bounds.bottom() + 1 - proposed.top());
    if (!maximumTrack.isEmpty()) nativeMaximum = nativeMaximum.boundedTo(maximumTrack.expandedTo(initial));
    const auto logicalMaximum = [dpr](const QSize& size) {
        // Largest integer logical size whose rounded native size still fits.
        return QSize(int(std::ceil((size.width() + 0.5) / dpr)) - 1,
                     int(std::ceil((size.height() + 0.5) / dpr)) - 1);
    };
    // Fit in logical pixels first, then convert the complete window once, as Qt does.
    const QSize maximum = logicalMaximum(nativeMaximum);
    const QSize size = fitWindowSize(QSizeF(display) * scale + QSizeF(chrome), display, chrome, minimum, maximum);
    const auto fits = [&](const QRect& rect) { return QRegion(rect).subtracted(allowed).isEmpty(); };
    if (fits(anchored(size * dpr))) return anchored(size * dpr);

    // Search nested, aspect-correct rectangles to respect gaps and unequal monitor heights.
    const bool wide = display.width() >= display.height();
    const QSize smallest = fitWindowSize(minimum, display, chrome, minimum, maximum) - chrome;
    int low = wide ? smallest.width() : smallest.height();
    int high = wide ? size.width() - chrome.width() : size.height() - chrome.height();
    const auto rectangleAt = [&](int extent) {
        const QSize viewport = wide
            ? QSize(extent, qMax(1, qRound(double(extent) * display.height() / display.width())))
            : QSize(qMax(1, qRound(double(extent) * display.width() / display.height())), extent);
        return anchored((viewport + chrome) * dpr);
    };
    while (low < high) {
        const int middle = low + (high - low + 1) / 2;
        if (fits(rectangleAt(middle))) low = middle;
        else high = middle - 1;
    }
    return rectangleAt(low);
}
}
