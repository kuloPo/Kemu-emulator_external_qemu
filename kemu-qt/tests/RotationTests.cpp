#include "../src/RotationEvents.h"
#include "../src/WindowSizing.h"
#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
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
