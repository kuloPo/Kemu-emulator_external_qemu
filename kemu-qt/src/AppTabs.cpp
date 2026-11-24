#include "AppTabs.h"
#include "android/emulation/KemuTabPipe.h"
#include "android/skin/qt/emulator-qt-window.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHoverEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPixmapCache>
#include <QTabBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionTab>
#include <QToolButton>

namespace kemu {
class ApplicationTabBar final : public QTabBar {
public:
    ApplicationTabBar(QToolButton* desktop, QWidget* parent)
        : QTabBar(parent), mDesktop(desktop) {}

protected:
    void initStyleOption(QStyleOptionTab* option, int index) const override {
        QTabBar::initStyleOption(option, index);
        if (mDesktop->isChecked()) option->state &= ~QStyle::State_Selected;
    }

private:
    QToolButton* mDesktop;
};

static QIcon applicationIcon(const QString& token, QStyle* style) {
    const auto key = QStringLiteral("kemu-tab/") + token;
    QPixmap pixmap;
    if (!QPixmapCache::find(key, &pixmap)) {
        const auto png = tabIconPng(token.toStdString());
        if (!pixmap.loadFromData(reinterpret_cast<const uchar*>(png.data()), png.size(), "PNG")) {
            return style->standardIcon(QStyle::SP_FileIcon);
        }
        QPixmapCache::insert(key, pixmap);
    }
    return QIcon(pixmap);
}

AppTabs::AppTabs(QHBoxLayout* title, QWidget* container, EmulatorQtWindow* display)
    : QObject(container), mDesktop(new QToolButton(title->parentWidget())),
      mBar(new ApplicationTabBar(mDesktop, title->parentWidget())), mDisplay(display) {
    const auto desktop = QCoreApplication::translate("Kemu", "Desktop");
    mDesktop->setObjectName("KemuDesktopTab");
    mDesktop->setAccessibleName(desktop);
    mDesktop->setToolTip(desktop);
    mDesktop->setFocusPolicy(Qt::NoFocus);
    mDesktop->setCheckable(true);
    mDesktop->setChecked(true);
    mDesktop->setEnabled(false);
    mDesktop->setFixedSize(28, 28);
    mDesktop->setIcon(QIcon(":/kemu/tab-home.svg"));
    mDesktop->setIconSize(QSize(16, 16));
    mDesktop->setStyleSheet(
        "QToolButton { background:transparent; border:1px solid #495666; border-radius:6px; padding:0; }"
        "QToolButton:hover { border-color:#718399; }"
        "QToolButton:checked { background:#344151; border-color:#344151; }");
    title->addWidget(mDesktop);
    title->addWidget(mBar);
    mBar->setObjectName("KemuAppTabs");
    mBar->setFocusPolicy(Qt::NoFocus);
    mBar->setDrawBase(false);
    mBar->setAttribute(Qt::WA_Hover);
    mBar->installEventFilter(this);
    mBar->setIconSize(QSize(13, 13));
    mBar->setExpanding(false);
    mBar->setElideMode(Qt::ElideRight);
    mBar->setStyleSheet(
        "QTabBar::tab { background:transparent; color:#b9c3d1; border:1px solid #495666; border-radius:6px;"
        " padding:0 7px; width:148px; height:26px; margin-right:3px; }"
        "QTabBar::tab:hover { border-color:#718399; color:#f0f4fa; }"
        "QTabBar::tab:selected { background:#344151; border-color:#344151; color:#f6f8fc; }"
        "QToolButton#KemuTabAudio { background:transparent; border:0; border-radius:8px; padding:0; }"
        "QToolButton#KemuTabAudio:hover { background:#526174; }"
        "QToolButton#KemuTabAudio:checked:hover { background:#576b89; }"
        "QToolButton#KemuTabClose { background:transparent; border:0; border-radius:4px; }"
        "QToolButton#KemuTabClose:hover { background:#526174; }"
        "QToolButton#KemuTabClose:pressed { background:#657488; }");
    mBar->hide();
    mDisplay->enableAppTabs();
    connect(mDesktop, &QToolButton::clicked, this, &AppTabs::home);
    connect(mDesktop, &QToolButton::toggled, mBar, [this] { mBar->update(); });
    auto select = [this](int index) {
        if (index >= 0) request("select", mBar->tabData(index).toJsonObject());
    };
    connect(mBar, &QTabBar::tabBarClicked, this, select);
    connect(mBar, &QTabBar::currentChanged, this, select);
    connect(&mPoll, &QTimer::timeout, this, &AppTabs::receive);
    mPoll.start(20);
}

void AppTabs::request(const char* operation, const QJsonObject& tab) {
    if (mEpoch.isEmpty() || !tab.contains("slot")) return;
    const int slot = tab.value("slot").toInt();
    QJsonObject command{{"epoch", mEpoch}, {"op", operation}, {"slot", slot},
                        {"token", tab.value("token")}};
    if (qstrcmp(operation, "mute") == 0) {
        command.insert("muted", tab.value("muted"));
        command.insert("seq", ++mSequence);
        sendTabCommand(QJsonDocument(command).toJson(QJsonDocument::Compact).toStdString());
        return;
    }
    if (qstrcmp(operation, "select") == 0) {
        if (mPendingSelect == slot) return;
        mPendingSelect = slot;
    } else {
        mPendingSelect = -1;
    }
    mDisplay->setTabInput(-1);
    if (mPendingSelect == slot || qstrcmp(operation, "home") == 0) present(tab);
    mPresentation = {};
    command.insert("seq", ++mSequence);
    sendTabCommand(QJsonDocument(command).toJson(QJsonDocument::Compact).toStdString());
}

void AppTabs::home() { request("home", QJsonObject{{"slot", 0}}); }

void AppTabs::updateTabs(const QJsonArray& tabs) {
    showCloseButton(-1);
    QHash<QString, QJsonObject> incoming;
    for (const auto entry : tabs) {
        const auto tab = entry.toObject();
        incoming.insert(tab.value("token").toString(), tab);
    }
    // Guest slot numbers can be reused. Keep surviving tabs in their visual order.
    for (int i = mBar->count() - 1; i >= 0; --i) {
        const auto token = mBar->tabData(i).toJsonObject().value("token").toString();
        const auto tab = incoming.take(token);
        if (tab.isEmpty()) {
            mBar->removeTab(i);
            continue;
        }
        mBar->setTabData(i, tab);
        mBar->setTabText(i, tab.value("label").toString());
        auto* logo = mBar->tabButton(i, QTabBar::LeftSide)->findChild<QLabel*>("KemuTabIcon");
        logo->setPixmap(applicationIcon(token, mBar->style()).pixmap(QSize(64, 64)));
    }
    for (const auto entry : tabs) {
        const auto tab = entry.toObject();
        const auto token = tab.value("token").toString();
        if (!incoming.contains(token)) continue;
        const auto icon = applicationIcon(token, mBar->style());
        const auto index = mBar->addTab(tab.value("label").toString());
        mBar->setTabData(index, tab);
        auto* leading = new QWidget(mBar);
        const int size = mBar->iconSize().height();
        leading->setFixedSize(size, 18);
        auto* logo = new QLabel(leading);
        logo->setObjectName("KemuTabIcon");
        logo->setAttribute(Qt::WA_TransparentForMouseEvents);
        logo->setGeometry(0, (leading->height() - size) / 2 + 1, size, size);
        logo->setPixmap(icon.pixmap(QSize(64, 64)));
        logo->setScaledContents(true);
        auto* sound = new QToolButton(leading);
        sound->setObjectName("KemuTabAudio");
        sound->setGeometry(size + 3, 0, 18, 18);
        sound->setIconSize(QSize(13, 13));
        sound->setFocusPolicy(Qt::NoFocus);
        sound->setCheckable(true);
        sound->hide();
        mBar->setTabButton(index, QTabBar::LeftSide, leading);
        connect(sound, &QToolButton::clicked, this, [this, sound, tab](bool muted) {
            sound->setChecked(!muted);
            QJsonObject target = tab;
            target.insert("muted", muted);
            request("mute", target);
        });
        auto* space = new QWidget(mBar);
        space->setFixedSize(18, 18);
        auto* close = new QToolButton(space);
        close->setObjectName("KemuTabClose");
        close->setFixedSize(space->size());
        close->setIcon(QIcon(":/kemu/tab-close.svg"));
        close->setIconSize(QSize(12, 12));
        close->setFocusPolicy(Qt::NoFocus);
        close->setAccessibleName(QCoreApplication::translate("Kemu", "Close"));
        close->hide();
        mBar->setTabButton(index, QTabBar::RightSide, space);
        connect(close, &QToolButton::clicked, this, [this, tab] { request("close", tab); });
    }
    mTabs = tabs;
    if (mBar->underMouse()) showCloseButton(mBar->tabAt(mBar->mapFromGlobal(QCursor::pos())));
}

void AppTabs::updateAudio(const QJsonObject& audio) {
    for (int i = 0; i < mBar->count(); ++i) {
        auto* leading = mBar->tabButton(i, QTabBar::LeftSide);
        auto* sound = leading->findChild<QToolButton*>("KemuTabAudio");
        const QString token = mBar->tabData(i).toJsonObject().value("token").toString();
        const QString state = audio.value(token).toString();
        const bool visible = state == "playing" || state == "muted";
        const bool muted = state == "muted";
        sound->setChecked(muted);
        sound->setIcon(QIcon(muted ? ":/kemu/tab-muted.svg" : ":/kemu/tab-sound.svg"));
        const auto action = muted ? QCoreApplication::translate("Kemu", "Unmute tab")
                                  : QCoreApplication::translate("Kemu", "Mute tab");
        sound->setAccessibleName(action);
        if (sound->isHidden() == visible) {
            sound->setVisible(visible);
            leading->setFixedWidth(visible ? sound->geometry().right() + 1 : mBar->iconSize().width());
            mBar->setTabButton(i, QTabBar::LeftSide, leading);
            leading->setVisible(mBar->isTabVisible(i));
        }
    }
    mAudio = audio;
}

void AppTabs::showCloseButton(int index) {
    auto* space = index >= 0 ? mBar->tabButton(index, QTabBar::RightSide) : nullptr;
    auto* close = space ? space->findChild<QToolButton*>() : nullptr;
    if (mHoveredClose == close) return;
    if (mHoveredClose) mHoveredClose->hide();
    mHoveredClose = close;
    if (close) close->show();
}

bool AppTabs::eventFilter(QObject* object, QEvent* event) {
    if (object == mBar) {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverMove) {
            showCloseButton(mBar->tabAt(static_cast<QHoverEvent*>(event)->position().toPoint()));
        } else if (event->type() == QEvent::HoverLeave) {
            showCloseButton(-1);
        }
    }
    return false;
}

void AppTabs::present(const QJsonObject& tab) {
    mActive = tab.value("slot").toInt();
    mDesktop->setChecked(mActive == 0);
    mRotation = tab.value("rotation").toInt();
    if (mActive > 0) {
        mDisplay->configureTabTouchSurface(QSize(tab.value("width").toInt(), tab.value("height").toInt()));
    }
    mDisplay->showTabSurface(mActive, mRotation);
}

void AppTabs::receive() {
    const auto packet = takeTabSnapshot();
    if (!packet) return;
    if (packet->empty()) {
        mPendingSelect = -1;
        mEpoch.clear();
        mPresentation = {};
        mDesktop->setEnabled(false);
        mDesktop->setChecked(true);
        mBar->hide();
        mActive = 0;
        mDisplay->setTabInput(-1);
        mDisplay->showTabSurface(0, 0);
        return;
    }
    auto state = QJsonDocument::fromJson(QByteArray::fromStdString(*packet)).object();
    const QString epoch = state.value("epoch").toString();
    if (epoch.isEmpty() || !state.value("tabs").isArray()) return;
    const bool reconnect = epoch != mEpoch;
    if (reconnect) {
        mEpoch = epoch;
        mSequence = 0;
    }
    if (state.value("seq").toInt() < mSequence) return;
    mPendingSelect = -1;
    const QSignalBlocker block(mBar);
    const auto tabs = state.value("tabs").toArray();
    const bool tabsChanged = reconnect || mTabs != tabs;
    if (tabsChanged) updateTabs(tabs);
    const auto audio = state.take("audio").toObject();
    if (tabsChanged || mAudio != audio) updateAudio(audio);
    state.remove("seq");
    if (!reconnect && state == mPresentation) return;
    mPresentation = state;
    mDesktop->setEnabled(true);
    mBar->setVisible(!tabs.isEmpty());
    // Select against the new bar width, or Qt can scroll earlier tabs out of view.
    if (tabsChanged) mBar->parentWidget()->layout()->activate();
    const int slot = state.value("active").toInt(-1);
    mDisplay->setTabInput(state.value("input").toInt(slot));
    if (slot < 0) return;
    if (slot == 0) {
        present(QJsonObject{{"slot", 0}});
        if (mDisplay->isActiveWindow()) mDisplay->setFocus();
        return;
    }
    for (int i = 0; i < mBar->count(); ++i) {
        const auto tab = mBar->tabData(i).toJsonObject();
        if (tab.value("slot").toInt() != slot) continue;
        mBar->setCurrentIndex(i);
        present(tab);
        if (mDisplay->isActiveWindow()) mDisplay->setFocus();
        break;
    }
}

void AppTabs::layout() {
    mDisplay->showTabSurface(mActive, mRotation);
}

void AppTabs::stop() {
    mPoll.stop();
    mDisplay->setTabInput(-1);
    mActive = 0;
    mDisplay->showTabSurface(0, 0);
}
}
