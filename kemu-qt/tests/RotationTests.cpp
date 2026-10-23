#include "../src/DisplayRotation.h"
#include "../src/WindowSizing.h"
#include <QCoreApplication>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        for (int rotation = 0; rotation < 4; ++rotation) {
            const QString viewport = QString("  Viewport INTERNAL: displayId=0, uniqueId=local:123, port=0, orientation=%1, logicalFrame=[0, 0, 1080, 1920]\r\n").arg(rotation);
            require(kemu::displayRotation(viewport + viewport) == rotation, "Primary display rotation was not parsed");
            require(kemu::displayRotation("Viewport EXTERNAL: displayId=1, orientation=2,\n" + viewport) == rotation,
                    "Secondary display changed the primary rotation");
        }
        for (const auto* invalid : {"", "Permission denied", "Viewport INTERNAL: displayId=0, orientation=4,",
                "Viewport INTERNAL: displayId=10, orientation=3,",
                "Viewport INTERNAL: displayId=0, orientation=1,\nViewport INTERNAL: displayId=0, orientation=3,"})
            require(!kemu::displayRotation(invalid), "Invalid or inconsistent rotation was accepted");
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
