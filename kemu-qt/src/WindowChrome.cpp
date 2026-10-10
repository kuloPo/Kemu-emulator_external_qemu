#include "WindowChrome.h"
#include "IconButton.h"
#include "AndroidMute.h"

#include <QApplication>
#include <QAccessibleWidget>
#include <QBoxLayout>
#include <QFile>
#include <QFont>
#include <QFontInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QMessageBox>
#include <QScreen>
#include <QWindow>
#include "android/console.h"
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
#ifdef KEMU_GUI_TESTS
void startLiveGuiChecks(EmulatorContainer*, EmulatorQtWindow*);
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
    container->setMinimumSize(856, 494);
    container->setAlignment(Qt::AlignCenter);
    container->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    container->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    container->setStyleSheet(
        "#KemuWindow { background: #15191f; border: none; }"
        "QToolTip { color: #edf1f6; background: #29323e; border: 1px solid #414d5e; padding: 5px; }");
    container->viewport()->setStyleSheet("background: #0c0e12;");
    container->setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
    mTitle = new QWidget(container);
    mTitle->setObjectName("KemuTitleBar");
    mTitle->installEventFilter(this);
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
    brand->setStyleSheet("color: #eef3fa; font-family: 'Microsoft YaHei UI'; font-size: 14px; font-weight: 600;");
    title->addWidget(brand);
    title->addStretch();
    auto* minimize = new IconButton(Icon::Minimize, QStringLiteral("最小化"), mTitle);
    mMaximize = new IconButton(Icon::Maximize, QStringLiteral("最大化"), mTitle);
    auto* close = new IconButton(Icon::Close, QStringLiteral("关闭"), mTitle);
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
    mSidebar->setObjectName("KemuSidebar");
    mSidebar->setStyleSheet("#KemuSidebar { background: #1b2028; }");
    auto* side = new QVBoxLayout(mSidebar);
    side->setContentsMargins(6, 10, 6, 10);
    side->setSpacing(5);
    const struct { Icon icon; const char* label; QtUICommand command; } specs[] = {
        {Icon::VolumeUp, "音量＋", QtUICommand::VOLUME_UP},
        {Icon::VolumeDown, "音量－", QtUICommand::VOLUME_DOWN},
        {Icon::Mute, "静音／取消静音", QtUICommand::VOLUME_DOWN},
        {Icon::Back, "返回", QtUICommand::BACK},
        {Icon::Home, "Home", QtUICommand::HOME},
        {Icon::Recents, "任务切换", QtUICommand::OVERVIEW}};
    for (const auto& spec : specs) {
        auto* button = new IconButton(spec.icon, QString::fromUtf8(spec.label), mSidebar);
        button->setEnabled(false);
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
                    button->setToolTip(error.isEmpty() ? QStringLiteral("静音／取消静音") : error);
                    if (!error.isEmpty()) qWarning("Kemu mute: %s", qPrintable(error));
                });
            } else {
                commands->handleUICommand(spec.command);
            }
            mDisplay->setFocus(Qt::OtherFocusReason);
        });
        side->addWidget(button);
        mButtons.append(button);
        if (spec.icon == Icon::Mute) {
            auto* separator = new QWidget(mSidebar);
            separator->setFixedHeight(1);
            separator->setStyleSheet("background: #303743;");
            side->addSpacing(5);
            side->addWidget(separator);
            side->addSpacing(5);
        }
    }
    side->addStretch();
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
    mLoadingText = new QLabel(QStringLiteral("启动中"), mLoading);
    mLoadingText->setObjectName("KemuLoadingText");
    mLoadingText->setAlignment(Qt::AlignCenter);
    mLoadingText->setStyleSheet("color: #d9dfe8; font-family: 'Microsoft YaHei UI'; font-size: 18px; font-weight: 500;");
    loadingRow->addWidget(mLoadingText);
    loadingRow->addStretch();
    loading->addLayout(loadingRow);
    loading->addStretch();
    mResize.setSingleShot(true);
    mResize.setInterval(0);
    connect(&mResize, &QTimer::timeout, this, [this] {
        if (!mHaveSurface || mClosing || mContainer->isMinimized()) return;
        const QSize available = mContainer->viewport()->size();
        if (available.isEmpty() || mRequestedSize == available) return;
        mRequestedSize = available;
        mDisplay->doResize(available);
    });
    connect(&mBootPoll, &QTimer::timeout, this, &WindowChrome::pollBoot);
    mBootPoll.start(100);
    const QRect available = container->screen()->availableGeometry();
    const int width = qMax(802, qMin(1280, qMin(available.width() - 112,
                                              (available.height() - 106) * 16 / 9)));
    container->resize(width + 54, width * 9 / 16 + 42);
    container->move(available.center() - container->rect().center());
    layout();
    mTitle->show();
    mSidebar->show();
    mLoading->show();
    QTimer::singleShot(0, this, &WindowChrome::checkAdb);
#ifdef _WIN32
    const HWND handle = reinterpret_cast<HWND>(container->winId());
    SetWindowLongPtrW(handle, GWL_STYLE, GetWindowLongPtrW(handle, GWL_STYLE) |
                      WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
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
#ifdef _WIN32
    if (mInstanceHandle) CloseHandle(mInstanceHandle);
#endif
}

void WindowChrome::layout() {
    if (mLayoutActive) return;
    mLayoutActive = true;
    mContainer->setViewportMargins(1, 41, 53, 1);
    const int width = mContainer->width(), height = mContainer->height();
    mTitle->setGeometry(1, 1, width - 2, 40);
    mSidebar->setGeometry(width - 53, 41, 52, qMax(0, height - 42));
    mLoading->setGeometry(1, 41, qMax(0, width - 54), qMax(0, height - 42));
    if (!mReady || mClosing) mLoading->raise();
    mLayoutActive = false;
    requestResize();
}

void WindowChrome::requestResize() {
    if (!mResize.isActive()) mResize.start();
}

void WindowChrome::surfaceChanged() {
    mHaveSurface = true;
    requestResize();
    if (!mReady || mClosing) mLoading->raise();
}

void WindowChrome::pollBoot() {
    if (mClosing || !mHaveSurface || !getConsoleAgents()->settings->guest_boot_completed()) return;
    mReady = true;
    mBootPoll.stop();
    mLoading->hide();
    for (auto* button : mButtons) button->setEnabled(true);
    mDisplay->setFocus(Qt::OtherFocusReason);
#ifdef KEMU_GUI_TESTS
    if (qEnvironmentVariableIsSet("KEMU_GUI_TEST_RESULTS")) startLiveGuiChecks(mContainer, mDisplay);
#endif
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
        QStringLiteral("未找到可用的 ADB"), QMessageBox::Ok, mContainer);
    warning->setObjectName("KemuAdbWarning");
    warning->setAttribute(Qt::WA_DeleteOnClose);
    warning->setWindowIcon(mContainer->windowIcon());
    warning->setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
    warning->setInformativeText(QStringLiteral(
        "APK 安装、文件传输等功能将不可用，模拟器仍可继续运行。\n\n"
        "请将 ADB 所在目录加入 PATH，或将 adb.exe 及配套 DLL 放入 Kemu.exe 同级目录下的 platform-tools，"
        "然后重新启动 Kemu。"));
    warning->button(QMessageBox::Ok)->setText(QStringLiteral("继续启动"));
    warning->open();
}

void WindowChrome::updateWindowState() {
    const bool maximized = mContainer->isMaximized();
    mMaximize->setIconType(maximized ? Icon::Restore : Icon::Maximize);
    const QString label = maximized ? QStringLiteral("还原") : QStringLiteral("最大化");
    mMaximize->setToolTip(label);
    mMaximize->setAccessibleName(label);
    layout();
}

void WindowChrome::toggleMaximized() {
    if (mContainer->isMaximized()) mContainer->showNormal();
    else mContainer->showMaximized();
    updateWindowState();
}

bool WindowChrome::eventFilter(QObject* watched, QEvent* event) {
    if (watched == mTitle) {
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
    if (mClosing) return;
    mClosing = true;
    mBootPoll.stop();
    mResize.stop();
    mMute->cancel();
    for (auto* button : mButtons) button->setEnabled(false);
    mLoadingText->setText(QStringLiteral("关闭中"));
    mLoading->show();
    mLoading->raise();
}

bool WindowChrome::nativeEvent(void* message, qintptr* result) {
#ifdef _WIN32
    if (!result) return false;
    auto* msg = static_cast<MSG*>(message);
    if (msg->message == WM_NCCALCSIZE && msg->wParam) { *result = 0; return true; }
    if (msg->message == WM_NCACTIVATE) { *result = TRUE; return true; }
    if (msg->message == WM_GETMINMAXINFO) {
        auto* info = reinterpret_cast<MINMAXINFO*>(msg->lParam);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            info->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top};
            info->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
        }
        info->ptMinTrackSize = {LONG(mContainer->minimumWidth() * mContainer->devicePixelRatioF()),
                                LONG(mContainer->minimumHeight() * mContainer->devicePixelRatioF())};
        *result = 0;
        return true;
    }
    if (msg->message == WM_NCHITTEST) {
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
