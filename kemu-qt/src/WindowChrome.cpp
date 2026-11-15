#include "WindowChrome.h"
#include "WindowSizing.h"
#include "UiFont.h"
#include "UiLanguage.h"
#include "IconButton.h"
#include "AndroidMute.h"
#include "AndroidRotation.h"
#include "FrameRateCounter.h"
#include "FrameRateLabel.h"
#ifdef _WIN32
#include "UpdateController.h"
#endif

#include <QApplication>
#include <QAccessibleWidget>
#include <QBoxLayout>
#include <QCursor>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMessageBox>
#include <QScreen>
#include <QSettings>
#include <QWindow>
#include <cmath>
#include "android/console.h"
#include "android/hw-sensors.h"
#include "host-common/hw-config.h"
#include "host-common/opengles.h"
#include "android/emulation/control/globals_agent.h"
#include "android/emulation/control/adb/AdbInterface.h"
#include "android/skin/qt/emulator-container.h"
#include "android/skin/qt/emulator-qt-window.h"
#include "android/skin/qt/tool-window.h"
#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

static void initKemuResources() { Q_INIT_RESOURCE(kemu); }

namespace kemu {
class FrameRateMonitor final {
public:
    FrameRateMonitor(gfxstream::RendererPtr renderer, int maxFrameRate)
        : mRenderer(std::move(renderer)), mCounter(FrameRateCounter::Clock::now(), maxFrameRate),
          mListener([this](gfxstream::FrameBufferChangeEvent event) {
            if (event.change == gfxstream::FrameBufferChange::FrameReady) mCounter.recordFrame();
        }) {
        mRenderer->addListener(&mListener);
    }
    ~FrameRateMonitor() { mRenderer->removeListener(&mListener); }
    double sample() { return mCounter.sample(); }

private:
    gfxstream::RendererPtr mRenderer;
    FrameRateCounter mCounter;
    gfxstream::Renderer::FrameBufferChangeEventListener mListener;
};

#ifdef KEMU_GUI_TESTS
void startLiveGuiChecks(EmulatorContainer*, EmulatorQtWindow*);
void startMuteGuiChecks(EmulatorContainer*);
void startFontGuiChecks(EmulatorContainer*);
void startFullScreenGuiChecks(EmulatorContainer*, EmulatorQtWindow*);
void startAffinityGuiChecks(EmulatorContainer*);
void startRotationGuiChecks(EmulatorContainer*, EmulatorQtWindow*);
void startResizeGuiChecks(EmulatorContainer*, EmulatorQtWindow*);
void startSensorRotationGuiChecks(EmulatorContainer*, EmulatorQtWindow*);
#endif
bool enabled() {
#ifdef _WIN32
    static const bool active = qEnvironmentVariable("KEMU_UI") == "1";
    return active;
#else
    return false;
#endif
}

WindowChrome::WindowChrome(EmulatorContainer* container, EmulatorQtWindow* display)
    : QObject(container), mContainer(container), mDisplay(display) {
    initializeUiTranslations();
    initKemuResources();
    // The renderer asks effectiveWinId() for its parent. Give the display its
    // own native handle so GPU coordinates follow its centered position rather
    // than the larger scroll-area viewport (which may have letterboxing).
    display->setAttribute(Qt::WA_NativeWindow);
    container->setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                              Qt::WindowMinMaxButtonsHint);
    container->setWindowTitle("Kemu");
    container->setWindowIcon(QIcon(":/kemu/kemu.ico"));
    container->setObjectName("KemuWindow");
    // QAccessibleScrollArea otherwise exposes only the viewport, excluding
    // the title/sidebar living in its margins.
    QAccessible::installFactory([](const QString&, QObject* object) -> QAccessibleInterface* {
        if (object->objectName() == "KemuWindow")
            return new QAccessibleWidget(static_cast<QWidget*>(object), QAccessible::Window);
        return nullptr;
    });
    container->setAlignment(Qt::AlignCenter);
    container->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    container->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    container->setStyleSheet(
        "#KemuWindow { background: #15191f; border: none; }"
        "QToolTip { color: #edf1f6; background: #29323e; border: 1px solid #414d5e; padding: 5px; }");
    container->viewport()->setStyleSheet("background: #0c0e12;");
    mTitle = new QWidget(container);
    mTitle->setObjectName("KemuTitleBar");
    mTitle->setAttribute(Qt::WA_NativeWindow);
    mTitle->setStyleSheet("#KemuTitleBar { background: #15191f; }");
    auto* title = new QHBoxLayout(mTitle);
    title->setContentsMargins(16, 0, 4, 0);
    title->setSpacing(4);
    auto* titleLogo = new QLabel(mTitle);
    titleLogo->setObjectName("KemuTitleLogo");
    titleLogo->setPixmap(container->windowIcon().pixmap(QSize(20, 20)));
    titleLogo->setFixedSize(20, 20);
    titleLogo->setAttribute(Qt::WA_TransparentForMouseEvents);
    title->addWidget(titleLogo);
    title->addSpacing(6);
    auto* brand = new QLabel("Kemu", mTitle);
    brand->setAttribute(Qt::WA_TransparentForMouseEvents);
    brand->setStyleSheet("color: #eef3fa;");
    title->addWidget(brand);
    title->addStretch();
    mFullScreenButton = new IconButton(Icon::FullScreen,
        QCoreApplication::translate("Kemu", "Full screen (F11)"), mTitle);
    mFullScreenButton->setObjectName("KemuFullScreen");
    title->addWidget(mFullScreenButton);
    auto* separator = new QWidget(mTitle);
    separator->setObjectName("KemuFullScreenSeparator");
    separator->setFixedSize(1, 20);
    separator->setStyleSheet("background: #303743;");
    title->addWidget(separator);
    connect(mFullScreenButton, &QAbstractButton::clicked, this, &WindowChrome::toggleFullScreen);
    auto* minimize = new IconButton(Icon::Minimize, QCoreApplication::translate("Kemu", "Minimize"), mTitle);
    mMaximize = new IconButton(Icon::Maximize, QCoreApplication::translate("Kemu", "Maximize"), mTitle);
    auto* close = new IconButton(Icon::Close, QCoreApplication::translate("Kemu", "Close"), mTitle);
    minimize->setObjectName("KemuMinimize");
    mMaximize->setObjectName("KemuMaximize");
    close->setObjectName("KemuClose");
    title->addWidget(minimize);
    title->addWidget(mMaximize);
    title->addWidget(close);
    connect(minimize, &QAbstractButton::clicked, container, &QWidget::showMinimized);
    connect(mMaximize, &QAbstractButton::clicked, this, [this] {
        toggleMaximized();
    });
    connect(close, &QAbstractButton::clicked, container, &QWidget::close);

    mSidebar = new QWidget(container);
    mMute = new AndroidMute(this);
    mRotation = new AndroidRotation(this);
    mSidebar->setObjectName("KemuSidebar");
    mSidebar->setAttribute(Qt::WA_NativeWindow);
    mSidebar->setStyleSheet("#KemuSidebar { background: #1b2028; }");
    auto* side = new QVBoxLayout(mSidebar);
    side->setContentsMargins(6, 10, 6, 10);
    side->setSpacing(5);
    const struct { Icon icon; const char* label; QtUICommand command; } specs[] = {
        {Icon::VolumeUp, QT_TRANSLATE_NOOP("Kemu", "Volume up"), QtUICommand::VOLUME_UP},
        {Icon::VolumeDown, QT_TRANSLATE_NOOP("Kemu", "Volume down"), QtUICommand::VOLUME_DOWN},
        {Icon::Mute, QT_TRANSLATE_NOOP("Kemu", "Mute / Unmute"), QtUICommand::VOLUME_DOWN},
        {Icon::Back, QT_TRANSLATE_NOOP("Kemu", "Back"), QtUICommand::BACK},
        {Icon::Home, QT_TRANSLATE_NOOP("Kemu", "Home"), QtUICommand::HOME},
        {Icon::Recents, QT_TRANSLATE_NOOP("Kemu", "Recent apps"), QtUICommand::OVERVIEW}};
    for (const auto& spec : specs) {
        auto* button = new IconButton(spec.icon, QCoreApplication::translate("Kemu", spec.label), mSidebar);
        button->setProperty("kemuLabel", spec.label);
        button->setEnabled(false);
        if (spec.icon == Icon::Mute) mMuteButton = button;
        if (spec.icon == Icon::VolumeUp || spec.icon == Icon::VolumeDown) {
            button->setAutoRepeat(true);
            button->setAutoRepeatDelay(400);
            button->setAutoRepeatInterval(180);
        }
        connect(button, &QAbstractButton::clicked, this, [this, spec, button] {
            auto* commands = mDisplay->toolWindow();
            if (spec.icon == Icon::Mute) {
                button->setEnabled(false);
                mMute->send([this, button](const QString& error) {
                    button->setEnabled(!mClosing);
                    button->setToolTip(error.isEmpty() ? QCoreApplication::translate("Kemu", "Mute / Unmute") : error);
                    if (!error.isEmpty()) qWarning("Kemu mute: %s", qPrintable(error));
                });
            } else {
                commands->handleUICommand(spec.command);
            }
            mDisplay->setFocus(Qt::OtherFocusReason);
        });
        side->addWidget(button);
        mButtons.append(button);
        if (spec.icon == Icon::Mute || spec.icon == Icon::Recents) {
            auto* separator = new QWidget(mSidebar);
            separator->setFixedHeight(1);
            separator->setStyleSheet("background: #303743;");
            side->addSpacing(5);
            side->addWidget(separator);
            side->addSpacing(5);
        }
    }
    const auto* hardware = getConsoleAgents()->settings->hw();
    const bool portrait = qstrcmp(hardware->hw_initialOrientation, "portrait") == 0;
    mRotate = new IconButton(portrait ? Icon::RotatePortrait : Icon::RotateLandscape,
        QCoreApplication::translate("Kemu", "Rotate"), mSidebar);
    mRotate->setObjectName("KemuRotateDevice");
    mRotate->setProperty("kemuLabel", QT_TRANSLATE_NOOP("Kemu", "Rotate"));
    mRotate->setEnabled(false);
    connect(mRotate, &QAbstractButton::clicked, this, [this] {
        rotateDevice();
        mDisplay->setFocus(Qt::OtherFocusReason);
    });
    side->addWidget(mRotate);
    mButtons.append(mRotate);
    auto* installApk = new IconButton(Icon::InstallApk,
        QCoreApplication::translate("Kemu", "Install APK"), mSidebar);
    installApk->setObjectName("KemuInstallApk");
    installApk->setProperty("kemuLabel", QT_TRANSLATE_NOOP("Kemu", "Install APK"));
    installApk->setEnabled(false);
    connect(installApk, &QAbstractButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(mContainer,
            QCoreApplication::translate("Kemu", "Install APK"), {},
            QCoreApplication::translate("Kemu", "Android packages (*.apk)"));
        if (mClosing) return;
        mDisplay->setFocus(Qt::OtherFocusReason);
        if (!path.isEmpty()) mDisplay->runAdbInstall(path);
    });
    side->addWidget(installApk);
    mButtons.append(installApk);
    side->addStretch();
#ifdef _WIN32
    mUpdates = new UpdateController(container,
        [display] { return QString::fromStdString(display->getAdbInterface()->adbPath()); },
        [display] { return QString::fromStdString(display->getAdbInterface()->serialString()); });
    auto* settings = new IconButton(Icon::Settings, QCoreApplication::translate("Kemu", "Settings"), mSidebar);
    settings->setObjectName("KemuSettingsButton"); side->addWidget(settings);
    connect(settings, &QAbstractButton::clicked, mUpdates, &UpdateController::showSettings);
#endif
    // A native sibling of the viewport covers the GPU child during boot.
    mLoading = new QWidget(container);
    mLoading->setObjectName("KemuLoading");
    mLoading->setAttribute(Qt::WA_NativeWindow);
    mLoading->setStyleSheet("#KemuLoading { background: #0c0e12; }");
    auto* loading = new QVBoxLayout(mLoading);
    loading->addStretch();
    auto* loadingRow = new QHBoxLayout;
    loadingRow->setSpacing(12);
    loadingRow->addStretch();
    auto* loadingLogo = new QLabel(mLoading);
    loadingLogo->setObjectName("KemuLoadingLogo");
    loadingLogo->setPixmap(container->windowIcon().pixmap(QSize(36, 36)));
    loadingLogo->setFixedSize(36, 36);
    loadingRow->addWidget(loadingLogo);
    mLoadingText = new QLabel(QCoreApplication::translate("Kemu", "Starting"), mLoading);
    mLoadingText->setObjectName("KemuLoadingText");
    mLoadingText->setAlignment(Qt::AlignCenter);
    mLoadingText->setStyleSheet("color: #d9dfe8;");
    loadingRow->addWidget(mLoadingText);
    loadingRow->addStretch();
    loading->addLayout(loadingRow);
    loading->addStretch();
    // A native sibling stays above the renderer's native child window.
    mFrameRate = new FrameRateLabel(container);
    mFrameRate->setObjectName("KemuFrameRate");
    mFrameRate->setStyleSheet("#KemuFrameRate { color: #b8f59b; background: #15191f; padding: 4px 8px; }");
    mFrameRate->setFont(uiFont(10, QFont::DemiBold));
    mFrameRate->hide();
    connect(&mFrameRatePoll, &QTimer::timeout, this, &WindowChrome::updateFrameRate);
    // Observe F11 before the focused child can forward it to Android.
    qApp->installEventFilter(this);
    mFullScreenPoll.setInterval(50);
    connect(&mFullScreenPoll, &QTimer::timeout, this, &WindowChrome::updateFullScreenChrome);
    for (auto* timer : {&mTitleHide, &mSidebarHide}) {
        timer->setSingleShot(true);
        timer->setInterval(500);
    }
    connect(&mTitleHide, &QTimer::timeout, mTitle, &QWidget::hide);
    connect(&mSidebarHide, &QTimer::timeout, mSidebar, &QWidget::hide);
#ifdef _WIN32
    mUpdates->showFrameRateChanged = [this](bool show) { setShowFrameRate(show); };
#endif
    setShowFrameRate(QSettings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu")
        .value("general/showFrameRate", false).toBool());
    mResize.setSingleShot(true);
    mResize.setInterval(16);
    connect(&mResize, &QTimer::timeout, this, [this] {
        if (!mHaveSurface || mClosing || mContainer->isMinimized()) return;
        const QSize available = mContainer->viewport()->size();
        if (available.isEmpty() || mRequestedSize == available) return;
        mRequestedSize = available;
        mDisplay->doResize(available);
    });
    mDisplayRecovery.setSingleShot(true);
    mDisplayRecovery.setInterval(150);
    connect(&mDisplayRecovery, &QTimer::timeout, this, &WindowChrome::recoverDisplayGeometry);
    connect(&mBootPoll, &QTimer::timeout, this, &WindowChrome::pollBoot);
    mBootPoll.start(100);
    const QRect available = container->screen()->availableGeometry();
    const auto sizing = windowSizing(
        QSize(hardware->hw_lcd_width, hardware->hw_lcd_height), available.size());
    container->setMinimumSize(sizing.minimum);
    container->resize(sizing.initial);
    mSurfacePortrait = hardware->hw_lcd_width < hardware->hw_lcd_height;
    mNormalSizes[mSurfacePortrait] = sizing.initial;
    container->move(available.center() - container->rect().center());
    layout();
    brand->setFont(uiFont(10, QFont::DemiBold));
    mLoadingText->setFont(uiFont(13, QFont::Medium));
    mTitle->show();
    mSidebar->show();
    mLoading->show();
#ifdef _WIN32
    mUpdates->languageChanged = [this, minimize, close, settings] {
        const auto label = [](IconButton* button, const QString& text) {
            button->setToolTip(text); button->setAccessibleName(text);
        };
        label(minimize, QCoreApplication::translate("Kemu", "Minimize"));
        label(close, QCoreApplication::translate("Kemu", "Close"));
        label(settings, QCoreApplication::translate("Kemu", "Settings"));
        for (auto* button : mButtons) {
            const auto text = QCoreApplication::translate("Kemu", button->property("kemuLabel").toByteArray().constData());
            button->setToolTip(text); button->setAccessibleName(text);
        }
        mLoadingText->setText(QCoreApplication::translate("Kemu", "Starting"));
        updateWindowState();
    };
#endif
    QTimer::singleShot(0, this, &WindowChrome::checkAdb);
#ifdef _WIN32
    const HWND handle = reinterpret_cast<HWND>(container->winId());
    SetWindowLongPtrW(handle, GWL_STYLE, GetWindowLongPtrW(handle, GWL_STYLE) |
                      WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    connect(container->windowHandle(), &QWindow::screenChanged, this, [this](QScreen* screen) {
        // Update the minimum at once in every window state; only the size repair waits.
        const QSize display = mDisplay->backingSurfaceSize();
        if (screen && !display.isEmpty())
            mContainer->setMinimumSize(windowSizing(display, screen->availableGeometry().size()).minimum);
        watchScreen(screen);
        requestDisplayRecovery();
    });
    watchScreen(container->screen());
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen*) { requestDisplayRecovery(); });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) { requestDisplayRecovery(); });
    const DWMNCRENDERINGPOLICY noFrame = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(handle, DWMWA_NCRENDERING_POLICY, &noFrame, sizeof(noFrame));
    const QString instance = qEnvironmentVariable("KEMU_INSTANCE");
    if (!instance.isEmpty()) SetPropW(handle, reinterpret_cast<LPCWSTR>(instance.utf16()), HANDLE(1));
    // The launcher passes its instance lease to the engine, then exits.
    mInstanceHandle = reinterpret_cast<void*>(qEnvironmentVariable("KEMU_INSTANCE_HANDLE").toULongLong());
    if (mInstanceHandle) SetHandleInformation(mInstanceHandle, HANDLE_FLAG_INHERIT, 0);
#endif
    if (qEnvironmentVariableIsSet("KEMU_DIAGNOSTICS")) {
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &WindowChrome::writeDiagnostics);
        timer->start(100);
    }
}

WindowChrome::~WindowChrome() {
    mFrameRateMonitor.reset();
#ifdef _WIN32
    if (mInstanceHandle) CloseHandle(mInstanceHandle);
#endif
}

void WindowChrome::layout() {
    if (mLayoutActive) return;
    mLayoutActive = true;
    mContainer->setViewportMargins(mFullScreen ? QMargins() : QMargins(1, 41, 53, 1));
    const int width = mContainer->width(), height = mContainer->height();
    const int border = mFullScreen ? 0 : 1;
    mTitle->setGeometry(border, border, width - 2 * border, 40);
    mSidebar->setGeometry(width - 52 - border, 40 + border, 52, qMax(0, height - 40 - 2 * border));
    mLoading->setGeometry(mContainer->viewport()->geometry());
    if (!mReady || mClosing) mLoading->raise();
    layoutFrameRate();
    mLayoutActive = false;
    requestResize();
    if (mDisplayRecoveryPending) mDisplayRecovery.start();
}

void WindowChrome::requestResize() {
    if (!mResize.isActive()) mResize.start();
}

void WindowChrome::watchScreen(QScreen* screen) {
    for (const auto& connection : mScreenConnections) disconnect(connection);
    mScreenConnections.clear();
    if (!screen) return;
    mScreenConnections = {
        connect(screen, &QScreen::geometryChanged, this, &WindowChrome::requestDisplayRecovery),
        connect(screen, &QScreen::availableGeometryChanged, this, &WindowChrome::requestDisplayRecovery),
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, &WindowChrome::requestDisplayRecovery),
        connect(screen, &QScreen::physicalDotsPerInchChanged, this, &WindowChrome::requestDisplayRecovery)};
}

void WindowChrome::requestDisplayRecovery() {
    if (mClosing) return;
    mRequestedSize = {};
    mDisplayRecoveryPending = true;
    mDisplayRecovery.start();
}

// Snapped windows keep the size Windows assigns to their snap area.
static bool isArranged(QWidget* window) {
#ifdef _WIN32
    static const auto arranged = reinterpret_cast<decltype(&IsWindowArranged)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "IsWindowArranged"));
    return arranged && arranged(reinterpret_cast<HWND>(window->winId()));
#else
    Q_UNUSED(window);
    return false;
#endif
}

void WindowChrome::recoverDisplayGeometry() {
    if (mClosing || !mDisplayRecoveryPending || mContainer->isMinimized() || !mResizeStartSize.isEmpty()) return;
    const QSize display = mDisplay->backingSurfaceSize();
    QScreen* screen = mContainer->screen();
    if (display.isEmpty() || !screen) return;
    const QRect available = screen->availableGeometry();
    // Maximized and full-screen windows are repaired once they return to the normal state.
    const bool normal = !mContainer->isMaximized() && !mContainer->isFullScreen();
    if (normal) {
        mRequestedSize = {};
        mDisplayRecoveryPending = false;
    }
    mContainer->setMinimumSize(windowSizing(display, available.size()).minimum);
    if (normal && !isArranged(mContainer)) {
        const QRect current = mContainer->geometry();
        // Only trim the settled normal size; an off-screen or spanning window is not pulled back.
        const QSize size = fitWindowSize(current.size(), display, current.size() - mContainer->viewport()->size(),
                                         mContainer->minimumSize(), current.size());
        if (size != current.size()) {
            // Keep an edge that touches the right or bottom of the work area there,
            // allowing one pixel of DPI rounding.
            QPoint origin = current.topLeft();
            if (qAbs(current.right() - available.right()) <= 1 && current.left() > available.left())
                origin.rx() += current.width() - size.width();
            if (qAbs(current.bottom() - available.bottom()) <= 1 && current.top() > available.top())
                origin.ry() += current.height() - size.height();
            mContainer->setGeometry(QRect(origin, size));
        }
    }
    requestResize();
}

void WindowChrome::surfaceChanged() {
    const bool portrait = mDisplay->width() < mDisplay->height();
    if (portrait != mSurfacePortrait) {
        // Until a deferred resize is applied, normalGeometry still belongs to
        // the orientation before maximizing or minimizing the window.
        if (mPendingWindowSize.isEmpty()) {
            const QRect normal = mContainer->normalGeometry();
            mPendingWindowCenter = normal.center();
            if (!normal.isEmpty()) mNormalSizes[mSurfacePortrait] = normal.size();
        }
        mSurfacePortrait = portrait;
        const auto sizing = windowSizing(mDisplay->backingSurfaceSize(), mContainer->screen()->availableGeometry().size());
        mContainer->setMinimumSize(sizing.minimum);
        mPendingWindowSize = mNormalSizes[portrait].isEmpty() ? sizing.initial : mNormalSizes[portrait];
        applyPendingWindowSize();
        mRequestedSize = {};
    }
    mHaveSurface = true;
    if (mDisplayRecoveryPending && !mDisplayRecovery.isActive()) mDisplayRecovery.start();
    requestResize();
    if (!mReady || mClosing) mLoading->raise();
    layoutFrameRate();
}

void WindowChrome::setShowFrameRate(bool show) {
    mFrameRatePoll.stop();
    mFrameRateMonitor.reset();
    mFrameRate->setText("FPS: --");
    if (show && !mClosing) mFrameRatePoll.start(1000);
    updateFrameRate();
}

void WindowChrome::updateFrameRate() {
    if (mFrameRatePoll.isActive() && mReady && !mClosing) {
        if (!mFrameRateMonitor) {
            const auto& renderer = android_getOpenglesRenderer();
            if (renderer) mFrameRateMonitor = std::make_unique<FrameRateMonitor>(
                renderer, getConsoleAgents()->settings->hw()->hw_lcd_vsync);
        } else {
            mFrameRate->setText(QString("FPS: %1").arg(qRound(mFrameRateMonitor->sample())));
        }
    }
    layoutFrameRate();
}

void WindowChrome::layoutFrameRate() {
    if (!mFrameRate) return;
    const bool visible = mFrameRatePoll.isActive() && mReady && !mClosing;
    if (visible) {
        mFrameRate->adjustSize();
        const QRect surface(mDisplay->mapTo(mContainer, QPoint()), mDisplay->size());
        const QRect bounds = surface.intersected(mContainer->viewport()->geometry());
        mFrameRate->move(bounds.topLeft() + QPoint(8, 8));
    }
    mFrameRate->setVisible(visible);
    if (visible) mFrameRate->raise();
    raiseChrome();
}

void WindowChrome::raiseChrome() {
    // Native siblings must stay above the renderer's native child window.
    if (mSidebar->isVisible()) mSidebar->raise();
    if (mTitle->isVisible()) mTitle->raise();
}

void WindowChrome::updateFullScreenChrome() {
    if (!mFullScreen || mClosing) return;
    if (!mContainer->isActiveWindow() || mContainer->isMinimized()) {
        mTitleHide.stop();
        mSidebarHide.stop();
        mTitle->hide();
        mSidebar->hide();
        return;
    }
    // The GPU child consumes mouse moves, so sample the cursor while full screen.
#ifdef _WIN32
    const HWND window = reinterpret_cast<HWND>(mContainer->winId());
    POINT pixel{};
    RECT bounds{};
    if (!GetCursorPos(&pixel) || !ScreenToClient(window, &pixel) || !GetClientRect(window, &bounds)) return;
    const bool inside = PtInRect(&bounds, pixel);
    // Keep subpixel coordinates: at high DPI, rounding the last physical pixel
    // to an integer logical position can put it outside the right/bottom edge.
    const qreal dpr = mContainer->devicePixelRatioF();
    const QPointF cursor(pixel.x / dpr, pixel.y / dpr);
#else
    const QPointF cursor = mContainer->mapFromGlobal(QCursor::pos());
    const bool inside = mContainer->rect().contains(cursor.toPoint());
#endif
    const auto reveal = [inside, cursor](QWidget* bar, QTimer& hide, bool edge) {
        if (inside && (edge || (bar->isVisible() && QRectF(bar->geometry()).adjusted(-8, -8, 8, 8).contains(cursor)))) {
            hide.stop();
            bar->show();
        } else if (bar->isVisible() && !hide.isActive() && !QApplication::mouseButtons()) {
            hide.start();
        }
    };
    reveal(mTitle, mTitleHide, cursor.y() < 8);
    reveal(mSidebar, mSidebarHide, cursor.x() >= mContainer->width() - 8);
    raiseChrome();
}

void WindowChrome::pollBoot() {
    if (mClosing || !mHaveSurface || !getConsoleAgents()->settings->guest_boot_completed()) return;
    mReady = true;
    mBootPoll.stop();
    mLoading->hide();
    updateFrameRate();
    for (auto* button : mButtons) button->setEnabled(button != mMuteButton);
    restoreMute();
    mDisplay->setFocus(Qt::OtherFocusReason);
    mRotation->start([this](int rotation) {
        if (mClosing || rotation == mGuestRotation) return;
        mGuestRotation = rotation;
        mRequestedSize = {};
        // Android rotates its logical display counterclockwise relative to
        // the framebuffer; the host skin must apply the inverse transform.
        mDisplay->rotateSkin(static_cast<SkinRotation>((4 - rotation) % 4));
    });
#ifdef KEMU_GUI_TESTS
    if (qEnvironmentVariableIsSet("KEMU_FONT_TEST_RESULTS")) startFontGuiChecks(mContainer);
    if (qEnvironmentVariableIsSet("KEMU_FULLSCREEN_TEST_RESULTS")) startFullScreenGuiChecks(mContainer, mDisplay);
    if (qEnvironmentVariableIsSet("KEMU_ROTATION_TEST_RESULTS")) startRotationGuiChecks(mContainer, mDisplay);
    if (qEnvironmentVariableIsSet("KEMU_RESIZE_TEST_RESULTS")) startResizeGuiChecks(mContainer, mDisplay);
    if (qEnvironmentVariableIsSet("KEMU_SENSOR_ROTATION_TEST_RESULTS")) startSensorRotationGuiChecks(mContainer, mDisplay);
    if (qEnvironmentVariableIsSet("KEMU_GUI_TEST_RESULTS")) startLiveGuiChecks(mContainer, mDisplay);
    if (qEnvironmentVariableIsSet("KEMU_MUTE_TEST_RESULTS")) startMuteGuiChecks(mContainer);
    if (qEnvironmentVariableIsSet("KEMU_AFFINITY_TEST_REQUEST")) startAffinityGuiChecks(mContainer);
#endif
}

void WindowChrome::restoreMute() {
    if (mClosing) return;
    mMute->restore([this](const QString& error) {
        if (mClosing) return;
        if (!error.isEmpty()) {
            qWarning("Kemu mute restore: %s", qPrintable(error));
            QTimer::singleShot(1000, this, &WindowChrome::restoreMute);
            return;
        }
        mMuteButton->setEnabled(true);
    });
}

void WindowChrome::rotateDevice() {
    if (!mReady || mClosing) return;
    const auto* sensors = getConsoleAgents()->sensors;
    float x = 0.f, y = 0.f, z = 0.f;
    float* current[] = {&x, &y, &z};
    if (sensors->getPhysicalParameter(PHYSICAL_PARAMETER_ROTATION, current, 3,
            PARAMETER_VALUE_TYPE_TARGET) != PHYSICAL_PARAMETER_STATUS_OK) {
        qWarning("Kemu rotation: cannot read device pose");
        return;
    }
    // Use the target so rapid clicks toggle correctly during interpolation.
    const bool rotated = std::abs(z) < 45.f;
    const float target[] = {0.f, 0.f, rotated ? 90.f : 0.f};
    if (sensors->setPhysicalParameterTarget(PHYSICAL_PARAMETER_ROTATION, target, 3,
            PHYSICAL_INTERPOLATION_SMOOTH) != PHYSICAL_PARAMETER_STATUS_OK) {
        qWarning("Kemu rotation: cannot update device pose");
        return;
    }
    const auto* hardware = getConsoleAgents()->settings->hw();
    const bool portrait = (qstrcmp(hardware->hw_initialOrientation, "portrait") == 0) != rotated;
    mRotate->setIconType(portrait ? Icon::RotatePortrait : Icon::RotateLandscape);
}

void WindowChrome::checkAdb() {
    if (mAdbChecked || mClosing) return;
    mAdbChecked = true;
    auto* adb = mDisplay->getAdbInterface();
    // Use startup discovery instead of a stale path from the old Extended
    // Controls settings. Upstream probes candidates with `adb version` and
    // accepts only executables that report an ADB protocol version.
    adb->setCustomAdbPath({});
    mAdbPath = QString::fromStdString(adb->adbPath());
    if (!mAdbPath.isEmpty()) return;

    auto* warning = new QMessageBox(QMessageBox::Warning, QStringLiteral("Kemu"),
        QCoreApplication::translate("Kemu", "No usable ADB found"), QMessageBox::Ok, mContainer);
    warning->setObjectName("KemuAdbWarning");
    warning->setAttribute(Qt::WA_DeleteOnClose);
    warning->setWindowIcon(mContainer->windowIcon());
    warning->setInformativeText(QCoreApplication::translate("Kemu", "APK installation and file transfer are unavailable. The emulator can continue running.\n\n"
        "Add ADB to PATH, or place adb.exe and its DLLs in platform-tools next to Kemu.exe, then restart Kemu."));
    warning->button(QMessageBox::Ok)->setText(QCoreApplication::translate("Kemu", "Continue"));
    warning->open();
}

void WindowChrome::updateWindowState() {
    // Restore viewport margins before applying a rotation deferred in full screen.
    layout();
    applyPendingWindowSize();
    if (mDisplayRecoveryPending) mDisplayRecovery.start();
    const bool maximized = mContainer->isMaximized();
    mMaximize->setIconType(maximized ? Icon::Restore : Icon::Maximize);
    const QString label = maximized ? QCoreApplication::translate("Kemu", "Restore") : QCoreApplication::translate("Kemu", "Maximize");
    mMaximize->setToolTip(label);
    mMaximize->setAccessibleName(label);
    mMaximize->setEnabled(!mFullScreen);
    mFullScreenButton->setIconType(mFullScreen ? Icon::ExitFullScreen : Icon::FullScreen);
    const QString fullScreenLabel = mFullScreen
        ? QCoreApplication::translate("Kemu", "Exit full screen (F11)")
        : QCoreApplication::translate("Kemu", "Full screen (F11)");
    mFullScreenButton->setToolTip(fullScreenLabel);
    mFullScreenButton->setAccessibleName(fullScreenLabel);
}

void WindowChrome::applyPendingWindowSize() {
    if (mPendingWindowSize.isEmpty() || mContainer->isMaximized() ||
        mContainer->isMinimized() || mContainer->isFullScreen()) return;
    const QRect available = mContainer->screen()->availableGeometry();
    const QPoint center = mPendingWindowCenter;
    const QSize display = mDisplay->backingSurfaceSize();
    if (display.isEmpty()) return;
    mContainer->setMinimumSize(windowSizing(display, available.size()).minimum);
    const QSize size = fitWindowSize(mPendingWindowSize, display,
        mContainer->size() - mContainer->viewport()->size(), mContainer->minimumSize(), available.size());
    mPendingWindowSize = {};
    mContainer->resize(size);
    const QPoint origin = center - mContainer->rect().center();
    mContainer->move(qBound(available.left(), origin.x(), available.right() - size.width() + 1),
                     qBound(available.top(), origin.y(), available.bottom() - size.height() + 1));
}

void WindowChrome::toggleMaximized() {
    if (mFullScreen) return;
    if (mContainer->isMaximized()) mContainer->showNormal();
    else mContainer->showMaximized();
    updateWindowState();
}

void WindowChrome::toggleFullScreen() {
    if (mClosing) return;
    // Qt also reports full screen for maximized frameless windows on monitors
    // with no reserved taskbar area. Track the user's mode independently.
    if (!mFullScreen) {
        mRestoreMaximized = mContainer->isMaximized();
        if (mContainer->isFullScreen()) mContainer->showNormal();
    }
    mFullScreen = !mFullScreen;
    mTitleHide.stop();
    mSidebarHide.stop();
    mTitle->setVisible(!mFullScreen);
    mSidebar->setVisible(!mFullScreen);
    if (mFullScreen) {
        mFullScreenPoll.start();
        mContainer->showFullScreen();
    } else {
        mFullScreenPoll.stop();
        if (mRestoreMaximized) mContainer->showMaximized();
        else mContainer->showNormal();
    }
    updateWindowState();
    mDisplay->setFocus(Qt::OtherFocusReason);
}

bool WindowChrome::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ||
        event->type() == QEvent::KeyRelease) {
        auto* widget = qobject_cast<QWidget*>(watched);
        auto* key = static_cast<QKeyEvent*>(event);
        if (widget && widget->window() == mContainer && key->key() == Qt::Key_F11 &&
            key->modifiers() == Qt::NoModifier) {
            if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) toggleFullScreen();
            key->accept();
            return true;
        }
    }
    if (watched == mDisplay && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        layoutFrameRate();
    }
    if (watched == mTitle && !mFullScreen) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                toggleMaximized();
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && mContainer->windowHandle()) {
                // The title lives in a native child window. Its mouse events
                // need to start the top-level window's system move explicitly.
                return mContainer->windowHandle()->startSystemMove();
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

void WindowChrome::beginClose() {
#ifdef _WIN32
    if (mUpdates) mUpdates->stop();
#endif
    if (mClosing) return;
    mClosing = true;
    setShowFrameRate(false);
    mBootPoll.stop();
    mResize.stop();
    mDisplayRecovery.stop();
    mFullScreenPoll.stop();
    mTitleHide.stop();
    mSidebarHide.stop();
    mMute->cancel();
    mRotation->stop();
    for (auto* button : mButtons) button->setEnabled(false);
    mLoadingText->setText(QCoreApplication::translate("Kemu", "Closing"));
    mLoading->show();
    mLoading->raise();
}

bool WindowChrome::nativeEvent(void* message, qintptr* result) {
#ifdef _WIN32
    if (!result) return false;
    auto* msg = static_cast<MSG*>(message);
    if (msg->message == WM_NCCALCSIZE && msg->wParam) { *result = 0; return true; }
    if (msg->message == WM_NCACTIVATE) { *result = TRUE; return true; }
    if (msg->message == WM_DISPLAYCHANGE || msg->message == WM_DPICHANGED) requestDisplayRecovery();
    if (msg->message == WM_ENTERSIZEMOVE) {
        RECT rect{};
        mResizeStartSize = GetWindowRect(msg->hwnd, &rect)
            ? QSize(rect.right - rect.left, rect.bottom - rect.top) : QSize();
    }
    if (msg->message == WM_EXITSIZEMOVE) {
        mResizeStartSize = {};
        if (mDisplayRecoveryPending) mDisplayRecovery.start();
    }
    if (msg->message == WM_SIZING && !IsZoomed(msg->hwnd) && !mFullScreen) {
        Qt::Edges edges;
        switch (msg->wParam) {
            case WMSZ_LEFT: edges = Qt::LeftEdge; break;
            case WMSZ_RIGHT: edges = Qt::RightEdge; break;
            case WMSZ_TOP: edges = Qt::TopEdge; break;
            case WMSZ_BOTTOM: edges = Qt::BottomEdge; break;
            case WMSZ_TOPLEFT: edges = Qt::TopEdge | Qt::LeftEdge; break;
            case WMSZ_TOPRIGHT: edges = Qt::TopEdge | Qt::RightEdge; break;
            case WMSZ_BOTTOMLEFT: edges = Qt::BottomEdge | Qt::LeftEdge; break;
            case WMSZ_BOTTOMRIGHT: edges = Qt::BottomEdge | Qt::RightEdge; break;
            default: return false;
        }
        const QSize display = mDisplay->backingSurfaceSize();
        if (display.isEmpty()) return false;
        auto* rect = reinterpret_cast<RECT*>(msg->lParam);
        if (mResizeStartSize.isEmpty()) {
            RECT rect{};
            if (!GetWindowRect(msg->hwnd, &rect)) return false;
            mResizeStartSize = QSize(rect.right - rect.left, rect.bottom - rect.top);
        }
        QRegion workArea;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFO info{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(monitor, &info))
                *reinterpret_cast<QRegion*>(data) += QRect(info.rcWork.left, info.rcWork.top,
                    info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&workArea));
        if (workArea.isEmpty()) return false;
        const double dpr = mContainer->devicePixelRatioF();
        const QRect constrained = constrainWindowResize(
            QRect(rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top),
            edges, mResizeStartSize, display, mContainer->size() - mContainer->viewport()->size(),
            mContainer->minimumSize(), workArea, dpr, mMaximumTrackSize);
        *rect = {constrained.x(), constrained.y(),
                 constrained.x() + constrained.width(), constrained.y() + constrained.height()};
        *result = TRUE;
        return true;
    }
    if (msg->message == WM_GETMINMAXINFO) {
        auto* info = reinterpret_cast<MINMAXINFO*>(msg->lParam);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            info->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top};
            info->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
        }
        mMaximumTrackSize = QSize(info->ptMaxTrackSize.x, info->ptMaxTrackSize.y);
        const QSize minimum = mContainer->minimumSize() * mContainer->devicePixelRatioF();
        info->ptMinTrackSize = {minimum.width(), minimum.height()};
        *result = 0;
        return true;
    }
    if (msg->message == WM_NCHITTEST) {
        if (mFullScreen) { *result = HTCLIENT; return true; }
        POINT pixel{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        ScreenToClient(msg->hwnd, &pixel);
        const QPoint point(qRound(pixel.x / mContainer->devicePixelRatioF()),
                           qRound(pixel.y / mContainer->devicePixelRatioF()));
        if (!IsZoomed(msg->hwnd)) {
            const bool left = point.x() < 6, right = point.x() >= mContainer->width() - 6;
            const bool top = point.y() < 6, bottom = point.y() >= mContainer->height() - 6;
            if (top || bottom || left || right) {
                *result = top ? (left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP)
                    : bottom ? (left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM)
                    : left ? HTLEFT : HTRIGHT;
                return true;
            }
        }
        if (point.y() <= 40 && !qobject_cast<QAbstractButton*>(mContainer->childAt(point))) {
            *result = HTCAPTION;
            return true;
        }
    }
#endif
    return false;
}

void WindowChrome::writeDiagnostics() {
    const QRect surface(mDisplay->mapTo(mContainer, QPoint()), mDisplay->size());
    const QRect viewport = mContainer->viewport()->geometry();
    const auto rect = [](const QRect& r) { return QJsonObject{{"x",r.x()}, {"y",r.y()}, {"w",r.width()}, {"h",r.height()}}; };
    QJsonObject data{{"pid", qint64(QCoreApplication::applicationPid())}, {"qt", qVersion()},
        {"ready", mReady}, {"closing",mClosing}, {"maximized",mContainer->isMaximized()},
        {"fullScreen",mFullScreen}, {"titleVisible",mTitle->isVisible()},
        {"sidebarVisible",mSidebar->isVisible()},
        {"guestRotation",mGuestRotation}, {"surfacePortrait",mSurfacePortrait},
        {"maximizeTooltip",mMaximize->toolTip()}, {"window",rect(mContainer->geometry())},
        {"viewport",rect(viewport)}, {"surface",rect(surface)}, {"sidebar",rect(mSidebar->geometry())},
        {"dpr",mContainer->devicePixelRatioF()}, {"loading",mLoading->isVisible()},
        {"oldToolbarVisible",mDisplay->toolWindow()->isVisible()},
        {"loadingFont",QFontInfo(mLoadingText->font()).family()},
        {"loadingFontStyle",QFontInfo(mLoadingText->font()).styleName()}};
    data.insert("adbChecked", mAdbChecked);
    data.insert("adbPath", mAdbPath);
#ifdef _WIN32
    const HWND handle = reinterpret_cast<HWND>(mContainer->winId());
    wchar_t desktop[128]{};
    DWORD used=0, cloaked=0;
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &used);
    data.insert("desktop", QString::fromWCharArray(desktop));
    const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (input) {
        GetUserObjectInformationW(input, UOI_NAME, desktop, sizeof(desktop), &used);
        data.insert("inputDesktop", QString::fromWCharArray(desktop));
        CloseDesktop(input);
    }
    DwmGetWindowAttribute(handle, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    data.insert("cloaked", int(cloaked));
    data.insert("visible", bool(IsWindowVisible(handle)));
#endif
    QFile file(qEnvironmentVariable("KEMU_DIAGNOSTICS"));
    if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(data).toJson());
}
}  // namespace kemu
