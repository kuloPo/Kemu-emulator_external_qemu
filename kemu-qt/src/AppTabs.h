#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class EmulatorQtWindow;
class QHBoxLayout;
class QJsonObject;
class QTabBar;
class QToolButton;
class QWidget;

namespace kemu {
class DesktopFocusCheck;
class AppTabs final : public QObject {
public:
    AppTabs(QHBoxLayout* title, QWidget* container, EmulatorQtWindow* display);
    ~AppTabs() override;
    QTabBar* bar() const { return mBar; }
    void layout();
    void home();
    void stop();

private:
    void checkDesktopFocus(bool openHome);
    void cancelDesktopCheck();
    void receive();
    void request(const char* operation, const QJsonObject& tab);
    void updateTabs(const QJsonArray& tabs);
    void updateAudio(const QJsonObject& audio);
    void present(const QJsonObject& tab);
    void showCloseButton(int index);
    bool eventFilter(QObject* object, QEvent* event) override;
    QToolButton* mDesktop;
    QTabBar* mBar;
    EmulatorQtWindow* mDisplay;
    QString mEpoch;
    QJsonArray mTabs;
    QJsonObject mPresentation;
    QJsonObject mAudio;
    QPointer<QToolButton> mHoveredClose;
    int mSequence = 0;
    int mPendingSelect = -1;
    int mActive = 0;
    int mRotation = 0;
    QTimer mPoll;
    QTimer mDesktopRetry;
    DesktopFocusCheck* mDesktopCheck = nullptr;
};
}
