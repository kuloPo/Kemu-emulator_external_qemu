#include "../src/RotationEvents.h"
#include "../src/WindowSizing.h"
#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static const std::vector<Qt::Edges> kResizeHandles = {
    Qt::LeftEdge, Qt::RightEdge, Qt::TopEdge, Qt::BottomEdge,
    Qt::TopEdge | Qt::LeftEdge, Qt::TopEdge | Qt::RightEdge,
    Qt::BottomEdge | Qt::LeftEdge, Qt::BottomEdge | Qt::RightEdge};

static QRect dragged(const QRect& start, Qt::Edges edges, const QSize& delta) {
    QRect proposed = start;
    if (edges.testFlag(Qt::LeftEdge)) proposed.setLeft(proposed.left() - delta.width());
    if (edges.testFlag(Qt::RightEdge)) proposed.setRight(proposed.right() + delta.width());
    if (edges.testFlag(Qt::TopEdge)) proposed.setTop(proposed.top() - delta.height());
    if (edges.testFlag(Qt::BottomEdge)) proposed.setBottom(proposed.bottom() + delta.height());
    return proposed;
}

static void requireAspect(const QSize& size, const QSize& display) {
    if (qAbs(qint64(size.width()) * display.height() - qint64(size.height()) * display.width()) >
        qMax(display.width(), display.height()) / 2)
        throw std::runtime_error("Window lost the display aspect ratio: " + std::to_string(size.width()) +
            "x" + std::to_string(size.height()) + " for " + std::to_string(display.width()) + "x" + std::to_string(display.height()));
}

static void checkInteractiveResize() {
    for (const auto display : {QSize(1920,1080), QSize(1080,1920), QSize(1440,1080), QSize(1080,1080)}) {
        for (const double dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
            const QSize chrome(54,42), minimum = QSize(320,452) + chrome;
            const QSize initial = (display + chrome) * dpr;
            const QRect workArea(-10000,-10000,30000,30000);
            for (const auto edges : kResizeHandles) {
                for (const auto delta : {QSize(240,0), QSize(0,180), QSize(240,180), QSize(-240,-180), (chrome + QSize(10,10)) * dpr - initial}) {
                    const QRect proposed = dragged(QRect(QPoint(-600,-300), initial), edges, delta);
                    const QRect result = kemu::constrainWindowResize(proposed, edges, initial, display, chrome, minimum, workArea, dpr);
                    const QSize logical = result.size() / dpr;
                    requireAspect(logical - chrome, display);
                    require(logical * dpr == result.size(), "Native size does not round-trip through Qt logical pixels");
                    require(logical.width() >= minimum.width() && logical.height() >= minimum.height(),
                            "Interactive resize went below the minimum size");
                    require(edges.testFlag(Qt::LeftEdge) ? result.right() == proposed.right() : result.left() == proposed.left(),
                            "Interactive resize moved the opposite horizontal edge");
                    require(edges.testFlag(Qt::TopEdge) ? result.bottom() == proposed.bottom() : result.top() == proposed.top(),
                            "Interactive resize moved the opposite vertical edge");
                    if ((delta.width() > 0 || delta.height() > 0) && proposed.size() != initial)
                        require(result.width() > initial.width() && result.height() > initial.height(),
                                "Dragging a corner along one axis did not grow the window");
                }
            }
        }
    }
    const QRect proposed(100,200,1334,762);
    require(kemu::constrainWindowResize(proposed, Qt::RightEdge, proposed.size(), {}, QSize(54,42), QSize(374,494), QRect(0,0,1920,1080)) == proposed,
            "An unavailable display changed the resize rectangle");
}

static void checkResizeSequences() {
    const QSize chrome(54,42), minimum(154,142);
    const QRect workArea(-10000,-10000,30000,30000);
    for (const auto display : {QSize(1080,1920), QSize(1920,1080)}) {
        for (const double dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
            const QSize initial = (display.scaled(QSize(800,800), Qt::KeepAspectRatio) + chrome) * dpr;
            const QRect start(QPoint(-600,-300), initial);
            for (const auto edges : kResizeHandles) {
                for (const auto movement : {QSize(3,0), QSize(0,3), QSize(3,3)}) {
                    QSize previous;
                    for (int step = -60; step <= 60; ++step) {
                        const QRect proposed = dragged(start, edges, movement * step);
                        const QSize size = kemu::constrainWindowResize(proposed, edges, initial, display,
                            chrome, minimum, workArea, dpr).size();
                        requireAspect(size / dpr - chrome, display);
                        if (!previous.isEmpty())
                            require(size.width() >= previous.width() && size.height() >= previous.height(),
                                    "A continuous drag reversed the window size");
                        previous = size;
                    }
                    const QRect smaller = kemu::constrainWindowResize(dragged(start, edges, movement * -10),
                        edges, initial, display, chrome, minimum, workArea, dpr);
                    if (dragged(start, edges, movement).size() != initial)
                        require(smaller.width() < initial.width() && smaller.height() < initial.height(),
                                "A one-axis drag could not shrink the window");
                }
            }
        }
    }
}

static void checkResizeBounds() {
    const QSize chrome(54,42), display(1080,1920), minimum(374,611);
    for (const double dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        const QRect workArea(QPoint(-1600,100), QSize(1280,1040) * dpr);
        const QSize initial = QSize(504,842) * dpr;
        for (const auto edges : kResizeHandles) {
            for (const auto origin : {workArea.topLeft() + QPoint(100,100), workArea.bottomRight() - QPoint(20,20)}) {
                const QRect start(origin, initial);
                for (const int delta : {0, 2, -20, 2000}) {
                    const QRect proposed = dragged(start, edges, QSize(delta,delta) * dpr);
                    const QSize maximumTrack = QSize(900,900) * dpr;
                    const QRect result = kemu::constrainWindowResize(proposed, edges, initial, display, chrome,
                        minimum, workArea, dpr, maximumTrack);
                    require(QRegion(result).subtracted(QRegion(workArea).united(start)).isEmpty(),
                            "A resize added off-screen pixels");
                    require(edges.testFlag(Qt::LeftEdge) ? result.right() == start.right() : result.left() == start.left(),
                            "Bounds moved the opposite horizontal edge");
                    require(edges.testFlag(Qt::TopEdge) ? result.bottom() == start.bottom() : result.top() == start.top(),
                            "Bounds moved the opposite vertical edge");
                    if (delta == 0) require(result == start, "Starting a resize changed the window geometry");
                    if (delta > 0) require(result.width() >= initial.width() && result.height() >= initial.height(),
                                           "Growing an off-screen window made it smaller");
                    if (delta < 0) require(result.width() < initial.width() && result.height() < initial.height(),
                                           "An off-screen window could not shrink");
                    require(result.width() <= maximumTrack.width() && result.height() <= maximumTrack.height(),
                            "The window exceeded the native tracking limit");
                    requireAspect(result.size() / dpr - chrome, display);
                    require((result.size() / dpr).width() >= minimum.width() &&
                            (result.size() / dpr).height() >= minimum.height(), "Bounds broke the minimum size");
                }
            }
        }
    }
    for (const auto screen : {QSize(1280,720), QSize(800,600), QSize(3840,2160)}) {
        const QSize minSize = kemu::windowSizing(display, screen).minimum;
        const QSize size = kemu::fitWindowSize(QSize(504,842), display, chrome, minSize, screen);
        requireAspect(size - chrome, display);
        require(size.width() <= screen.width() && size.height() <= screen.height(),
                "Restored window exceeded a smaller screen");
    }
    for (int width = 900; width < 1400; ++width) {
        const QSize landscape(1920,1080), screen(1920,1040);
        const QSize requested = QSize(width, qRound(width * 1080.0 / 1920)) + chrome;
        const QSize fitted = kemu::fitWindowSize(requested, landscape, chrome, QSize(374,494), screen);
        require(fitted == requested, "Restoring an aspect-correct size changed its dimensions");
    }
}

static void checkMultipleMonitorResize() {
    const QSize chrome(54,42), landscape(1920,1080), portrait(1080,1920);
    for (const double dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        const auto native = [dpr](const QRect& rect) {
            return QRect(QPoint(qRound(rect.x() * dpr), qRound(rect.y() * dpr)), rect.size() * dpr);
        };
        const QRect primary = native(QRect(0,0,1920,1040));
        const QRegion horizontal = QRegion(primary).united(native(QRect(1920,0,1920,1040)));
        const QRegion left = QRegion(primary).united(native(QRect(-1920,0,1920,1040)));
        const QRegion vertical = QRegion(primary).united(native(QRect(0,1040,1920,1040)));
        const auto grow = [&](const QRect& start, Qt::Edges edges, const QSize& display,
                              const QRegion& screens, const QSize& delta) {
            const QRect result = kemu::constrainWindowResize(dragged(start, edges, delta * dpr), edges,
                start.size(), display, chrome, kemu::windowSizing(display, QSize(1920,1040)).minimum, screens, dpr);
            require(result.width() > start.width() && result.height() > start.height(),
                    "A window could not grow across monitors");
            require(edges.testFlag(Qt::LeftEdge) ? result.right() == start.right() : result.left() == start.left(),
                    "Crossing monitors moved the opposite horizontal edge");
            require(edges.testFlag(Qt::TopEdge) ? result.bottom() == start.bottom() : result.top() == start.top(),
                    "Crossing monitors moved the opposite vertical edge");
            require(QRegion(result).subtracted(screens).isEmpty(), "A cross-monitor resize escaped the screens");
            requireAspect(result.size() / dpr - chrome, display);
            return result;
        };
        grow(native(QRect(1400,100,1334,762)), Qt::LeftEdge, landscape, horizontal, QSize(2,0));
        grow(native(QRect(-500,100,1334,762)), Qt::LeftEdge, landscape, left, QSize(2,0));
        const QRect start = native(QRect(800,100,1000,574));
        const QRect across = grow(start, Qt::RightEdge, landscape, horizontal, QSize(300,0));
        require(across.right() > primary.right(), "A resize stopped at the monitor seam");
        const QRect below = grow(native(QRect(100,100,504,842)), Qt::BottomEdge, portrait, vertical, QSize(0,300));
        require(below.bottom() > primary.bottom(), "A resize stopped at a vertical monitor seam");
        for (const auto second : {QRect(1920,400,1920,1040), QRect(2020,0,1920,1040)}) {
            const QRegion screens = QRegion(primary).united(native(second));
            const QRect result = grow(start, Qt::RightEdge, landscape, screens, QSize(500,0));
            require(result.right() <= primary.right(), "A resize grew into a gap between monitors");
        }
        const QRect offscreen = native(QRect(100,498,504,842));
        const QRect limited = kemu::constrainWindowResize(dragged(offscreen, Qt::RightEdge, QSize(2,0) * dpr),
            Qt::RightEdge, offscreen.size(), portrait, chrome, QSize(374,610), primary, dpr, QSize(400,700) * dpr);
        require(limited == offscreen, "A smaller screen or tracking limit shrank an existing window");
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        checkInteractiveResize();
        checkResizeSequences();
        checkResizeBounds();
        checkMultipleMonitorResize();
        kemu::RotationEvents events;
        std::vector<int> received;
        const auto feed = [&](const std::string& bytes) {
            for (char ch : bytes)
                if (const auto rotation = events.accept(ch)) received.push_back(*rotation);
        };
        feed("KEMU_ROTATION_V1 ");
        require(received.empty(), "Partial event was accepted");
        feed("0\nKEMU_ROTATION_V1 0\nKEMU_ROTATION_V1 1\r\n");
        feed("Permission denied\nKEMU_ROTATION_V2 2\nKEMU_ROTATION_V1 4\n");
        feed("KEMU_ROTATION_V1 -1\nKEMU_ROTATION_V1 10\nKEMU_ROTATION_V1 2 extra\n");
        feed(std::string(10000, 'x') + "KEMU_ROTATION_V1 2\n");
        feed("KEMU_ROTATION_V1 2\nKEMU_ROTATION_V1 3\nKEMU_ROTATION_V1 0\n");
        require(received == std::vector<int>({0,1,2,3,0}), "Rotation events were lost, duplicated or malformed");
        events = kemu::RotationEvents();
        feed("KEMU_ROTATION_V1 0\n");
        require(received.size() == 6, "Reconnection did not publish the initial rotation");
        for (const auto screen : {QSize(1920,1040), QSize(1280,720), QSize(960,540)}) {
            const auto portrait = kemu::windowSizing(QSize(1080,1920), screen);
            require(portrait.initial.width() < portrait.initial.height(), "Portrait window is not upright");
            require(portrait.initial.width() <= screen.width() && portrait.initial.height() <= screen.height(),
                    "Portrait window exceeds the screen");
            require(portrait.minimum.width() <= portrait.initial.width() && portrait.minimum.height() <= portrait.initial.height(),
                    "Window minimum prevents portrait rotation");
        }
        std::cout << "Display rotation and window sizing checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
