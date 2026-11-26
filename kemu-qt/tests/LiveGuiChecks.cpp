// Opt-in checks run inside the real emulator UI thread, using the real GPU
// child window. Not included in the normal release build.
// KEMU_GUI_TEST_MUTE_APP limits the checks to the sidebar mute button once
// that package is in the foreground and playing audio.
#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFontInfo>
#include <QHoverEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QXmlStreamReader>
#include "android/emulation/KemuTabPipe.h"
#include "android/skin/linux_keycodes.h"
#include "android/skin/qt/emulator-container.h"
#include "android/skin/qt/emulator-qt-window.h"
#include "android/skin/qt/tool-window.h"
#include <windows.h>

namespace kemu {
class LiveGuiChecks final : public QObject {
public:
    LiveGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display)
        : QObject(root), mRoot(root), mDisplay(display) {
        mBefore = root->size();
        mMuteApp = qEnvironmentVariable("KEMU_GUI_TEST_MUTE_APP");
        mRecentsApp = qEnvironmentVariable("KEMU_GUI_TEST_RECENTS_APP");
        mSwitchApp = qEnvironmentVariable("KEMU_GUI_TEST_TAB_APP");
        if (!mMuteApp.isEmpty()) mStage = 14;
        mElapsed.start();
        connect(&mTimer, &QTimer::timeout, this, [this] { tick(); });
        mTimer.start(16);
    }
private:
    QAbstractButton* button(const char* source) {
        const auto name = QCoreApplication::translate("Kemu", source);
        for (auto* button : mRoot->findChildren<QAbstractButton*>())
            if (button->accessibleName() == name && button->isVisible()) return button;
        fail("Missing visible button: " + name);
        return nullptr;
    }
    bool fitted() {
        HWND render = nullptr;
        EnumChildWindows(reinterpret_cast<HWND>(mRoot->winId()), [](HWND child, LPARAM out) -> BOOL {
            wchar_t name[80]{};
            GetClassNameW(child, name, 80);
            if (wcscmp(name, L"subWin") == 0) { *reinterpret_cast<HWND*>(out) = child; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&render));
        if (!render) return false;
        DWORD renderPid = 0;
        GetWindowThreadProcessId(render, &renderPid);
        if (renderPid != GetCurrentProcessId()) return false;
        RECT rect{};
        GetWindowRect(render, &rect);
        const double dpr = mRoot->devicePixelRatioF();
        const QPoint origin = mDisplay->mapToGlobal(QPoint());
        const QSize expected = QSize(1920,1080).scaled(mRoot->viewport()->size(), Qt::KeepAspectRatio);
        mGeometry = QJsonObject{{"renderX",int(rect.left)}, {"renderY",int(rect.top)},
            {"renderWidth",int(rect.right-rect.left)}, {"renderHeight",int(rect.bottom-rect.top)},
            {"expectedX",qRound(origin.x()*dpr)}, {"expectedY",qRound(origin.y()*dpr)},
            {"expectedWidth",qRound(expected.width()*dpr)}, {"expectedHeight",qRound(expected.height()*dpr)}};
        return qAbs(mDisplay->width() - expected.width()) <= 1 && qAbs(mDisplay->height() - expected.height()) <= 1 &&
            qAbs(rect.left - qRound(origin.x()*dpr)) <= 2 && qAbs(rect.top - qRound(origin.y()*dpr)) <= 2 &&
            qAbs((rect.right-rect.left) - qRound(expected.width()*dpr)) <= 3 &&
            qAbs((rect.bottom-rect.top) - qRound(expected.height()*dpr)) <= 3;
    }
    QString adb(QStringList arguments) {
        QProcess process;
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
        process.start(qEnvironmentVariable("KEMU_TEST_ADB"), QStringList{"-s","emulator-5554"} + arguments);
        if (!process.waitForFinished(10000)) { process.kill(); process.waitForFinished(); fail("ADB observation timed out"); return {}; }
        if (process.exitCode()) fail("ADB observation failed");
        return QString::fromUtf8(process.readAllStandardOutput());
    }
    QString muted() {
        return QRegularExpression("STREAM_MUSIC:[\\s\\S]*?Muted: (true|false)").match(adb({"shell","dumpsys","audio"})).captured(1);
    }
    void fail(const QString& error) {
        if (mFailed) return;
        mFailed = true;
        mTimer.stop();
        mRoot->screen()->grabWindow(mRoot->winId()).save(qEnvironmentVariable("KEMU_GUI_TEST_RESULTS") + ".png");
        save(error);
    }
    void save(const QString& error = {}) {
        QJsonObject data{{"passed",error.isEmpty()}, {"error",error}, {"qt",qVersion()},
            {"pid",qint64(GetCurrentProcessId())}, {"checks",mChecks}, {"geometry",mGeometry}};
        QFile file(qEnvironmentVariable("KEMU_GUI_TEST_RESULTS"));
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(data).toJson());
    }
    void advance() { ++mStage; mElapsed.restart(); mStable=0; mFirstFit=-1; }
    void doubleClickTitle() {
        auto* title = mRoot->findChild<QWidget*>("KemuTitleBar");
        if (!title) { fail("Missing title drag surface"); return; }
        const QPointF local(150,20);
        QMouseEvent event(QEvent::MouseButtonDblClick, local, title->mapToGlobal(local.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(title, &event);
    }
    void tick() {
        if (mFailed) return;
        if (qEnvironmentVariableIsSet("KEMU_GUI_TEST_TAB_PEN") ||
            qEnvironmentVariableIsSet("KEMU_GUI_TEST_TAB_WHEEL")) { checkTabPointerInput(); return; }
        if (qEnvironmentVariableIsSet("KEMU_GUI_TEST_TAB_RECOVERY")) { checkTabRecovery(); return; }
        if (qEnvironmentVariableIsSet("KEMU_GUI_TEST_TAB_AUDIO")) { checkTabAudio(); return; }
        if (!mSwitchApp.isEmpty()) {
            checkTabSwitch();
            return;
        }
        if (!mRecentsApp.isEmpty()) {
            auto* tabs = mRoot->findChild<QTabBar*>("KemuAppTabs");
            auto* desktop = mRoot->findChild<QToolButton*>("KemuDesktopTab");
            if (mElapsed.elapsed() > (mStage == 0 ? 120000 : 10000)) {
                fail(mStage == 0 ? "Waiting for the recents test app" : mStage == 1
                        ? "Recent apps did not open on the desktop" : "Recent task did not return to its tab");
                return;
            }
            if (mStage == 0) {
                if (tabs && (!tabs->isVisible() || !desktop || desktop->isChecked() || tabs->currentIndex() < 0)) return;
                if (++mStable % 15) return;
                const auto windows = adb({"shell", "dumpsys", "window"});
                const auto focus = QRegularExpression("mCurrentFocus=[^\\n]*" +
                        QRegularExpression::escape(mRecentsApp) + "/");
                if (!focus.match(windows).hasMatch()) return;
                if (tabs) {
                    mRecentsLabel = tabs->tabText(tabs->currentIndex());
                    mRecentsTab = tabs->tabData(tabs->currentIndex()).toJsonObject();
                    gfxstream_display_view view{};
                    if (!getPresentationView(&view) || !view.colorBuffer ||
                            view.slot != mRecentsTab.value("slot").toInt()) return;
                    mChecks.append("Starting tab: " + mRecentsLabel);
                }
                if (auto* b = button("Recent apps")) b->click();
                advance();
            } else if (mStage == 1 && (!tabs || (desktop && desktop->isChecked())) &&
                       ++mStable % 15 == 0 && overviewVisible()) {
                mChecks.append("Sidebar opened Android Overview on the desktop display");
                if (tabs) {
                    if (!tapOverviewCard()) { fail("Missing recent task card: " + mRecentsLabel); return; }
                    advance();
                } else {
                    mTimer.stop();
                    save();
                }
            } else if (mStage == 2 && tabs && desktop && !desktop->isChecked() &&
                       tabs->currentIndex() >= 0 && ++mStable % 15 == 0) {
                const auto tab = tabs->tabData(tabs->currentIndex()).toJsonObject();
                if (tab.value("task") != mRecentsTab.value("task") ||
                        tab.value("display") != mRecentsTab.value("display")) return;
                const auto window = adb({"shell", "dumpsys", "window"});
                const auto display = QRegularExpression(QStringLiteral("mTopFocusedDisplayId=(\\d+)"))
                        .match(window);
                if (!display.hasMatch() || display.captured(1).toInt() != tab.value("display").toInt()) return;
                mChecks.append("Recent task card restored its original task, display and selected tab");
                mTimer.stop();
                save();
            }
            return;
        }
        if (!mMuteApp.isEmpty() && !mMuteAppReady) {
            if (mElapsed.elapsed() > 90000) { fail("Timed out waiting for the mute test app"); return; }
            if (++mStable % 30) return;
            const QString focus = QRegularExpression("mCurrentFocus=([^\\n]+)")
                .match(adb({"shell", "dumpsys", "window"})).captured(1);
            if (!focus.contains(mMuteApp + "/")) return;
            const QString pid = adb({"shell", "pidof", mMuteApp}).trimmed();
            const QString audio = adb({"shell", "dumpsys", "audio"});
            if (pid.isEmpty() || !QRegularExpression("u/pid:\\d+/" +
                    QRegularExpression::escape(pid) + " state:started").match(audio).hasMatch()) return;
            mMuteAppReady = true;
            mElapsed.restart();
            mChecks.append("Mute test foreground: " + focus);
        }
        if (mElapsed.elapsed() > 5000) { fail(QString("Stage %1 timed out; surface %2x%3, viewport %4x%5")
            .arg(mStage).arg(mDisplay->width()).arg(mDisplay->height()).arg(mRoot->viewport()->width()).arg(mRoot->viewport()->height())); return; }
        if (mStage <= 7) {
            if (!fitted()) {
                if (mStable) fail("GPU geometry oscillated after settling");
                return;
            }
            if (mFirstFit < 0) mFirstFit = mElapsed.elapsed();
            if (++mStable < 40) return;
            mChecks.append(QJsonObject{{"stage",mStage}, {"firstFitMs",mFirstFit}, {"stableSamples",mStable},
                {"width",mRoot->width()}, {"height",mRoot->height()}});
            if (mStage == 0) {
                if (mRoot->windowTitle() != "Kemu" || mDisplay->toolWindow()->isVisible()) { fail("Old chrome remains"); return; }
                for (const auto* name : {"KemuTitleLogo", "KemuLoadingLogo"}) {
                    const auto* logo = mRoot->findChild<QLabel*>(name);
                    if (!logo || logo->pixmap().isNull()) { fail(QString("Missing logo: %1").arg(name)); return; }
                }
                const auto* loadingText = mRoot->findChild<QLabel*>("KemuLoadingText");
                if (!loadingText || QFontInfo(loadingText->font()).family() != "Microsoft YaHei UI") {
                    fail("Startup font did not resolve to Microsoft YaHei UI"); return;
                }
                mChecks.append("title/startup logos loaded; startup font resolved to Microsoft YaHei UI");
                if (auto* b = button("Maximize")) b->click();
            } else if (mStage == 1) {
                if (!mRoot->isMaximized()) { fail("Maximize state did not update"); return; }
                if (auto* b = button("Restore")) b->click();
            } else if (mStage == 2) {
                if (mRoot->isMaximized() || mRoot->size() != mBefore) { fail("Restore geometry mismatch"); return; }
                mRoot->resize(1189,762);
            } else if (mStage == 3) mRoot->resize(900,520);
            else if (mStage == 4) mRoot->resize(1510,930);
            else if (mStage == 5) mRoot->resize(1001,611);
            else if (mStage == 6) mRoot->resize(mBefore);
            else {
                mRoot->screen()->grabWindow(mRoot->winId()).save(qEnvironmentVariable("KEMU_GUI_TEST_RESULTS") + ".png");
                if (auto* b = button("Minimize")) b->click();
            }
            advance();
        } else if (mStage == 8) {
            if (!mRoot->isMinimized()) { fail("Minimize failed"); return; }
            mRoot->showNormal();
            advance();
        } else if (mStage == 9) {
            if (!fitted()) return;
            if (auto* b = button("Home")) b->click();
            advance();
        } else if (mElapsed.elapsed() > 500) {
            if (mStage == 10) {
                const QString activity = adb({"shell","dumpsys","activity","activities"});
                if (!QRegularExpression("(?:mResumedActivity|topResumedActivity)[^\\n]*launcher", QRegularExpression::CaseInsensitiveOption).match(activity).hasMatch()) {
                    fail("Home did not resume launcher"); return;
                }
                if (auto* b = button("Recent apps")) b->click();
            } else if (mStage == 11) {
                if (!overviewVisible()) return;
                if (auto* b = button("Back")) b->click();
            }
            else if (mStage == 12) { if (auto* b = button("Volume up")) b->click(); }
            else if (mStage == 13) { if (auto* b = button("Volume down")) b->click(); }
            else if (mStage == 14) {
                mMutedBefore = muted();
                if (mMutedBefore.isEmpty()) { fail("Cannot observe Android mute state"); return; }
                if (auto* b = button("Mute / Unmute")) b->click();
            } else if (mStage == 15) {
                auto* b = button("Mute / Unmute");
                if (!b || !b->isEnabled() || muted() == mMutedBefore) return;
                b->click();
            } else if (mStage == 16) {
                auto* b = button("Mute / Unmute");
                if (!b || !b->isEnabled() || muted() != mMutedBefore) return;
                if (!mMuteApp.isEmpty()) {
                    mChecks.append("Sidebar mute button toggled and restored Android media mute");
                    mTimer.stop();
                    save();
                    return;
                }
                mChecks.append("minimize/restore; six internal key buttons; Home resumed launcher; mute toggled and restored");
                doubleClickTitle();
            } else if (mStage == 17) {
                if (!mRoot->isMaximized() || !fitted()) { fail("Title double-click did not maximize"); return; }
                doubleClickTitle();
            } else {
                if (mRoot->isMaximized() || !fitted()) { fail("Title double-click did not restore"); return; }
                mChecks.append("title double-click maximized and restored");
                mTimer.stop();
                save();
                QTimer::singleShot(200, mRoot, [this] { mRoot->close(); });
                return;
            }
            advance();
        }
    }
    void checkTabAudio() {
        if (mElapsed.elapsed() > (mStage == 0 ? 240000 : 30000)) {
            fail(QString("Tab audio stage %1 timed out").arg(mStage)); return;
        }
        if (++mStable % 8) return;
        auto* bar = mRoot->findChild<QTabBar*>("KemuAppTabs");
        if (!bar || bar->count() != 2 || bar->currentIndex() != 1) return;
        auto sound = [bar](int i) {
            return bar->tabButton(i, QTabBar::LeftSide)->findChild<QToolButton*>("KemuTabAudio");
        };
        auto* first = sound(0);
        auto* second = sound(1);
        if (!first || !second || !second->isVisible() || second->isChecked()) return;
        const auto pkg = bar->tabData(0).toJsonObject().value("package").toString();
        auto tone = [this, pkg](const QString& mode) {
            adb({"shell", "am", "broadcast", "-a", pkg + ".AUDIO", "-p", pkg, "--es", "mode", mode});
        };
        auto gain = [this](const QString& package) {
            const QString pid = adb({"shell","pidof",package}).trimmed();
            const QString tracks = adb({"shell","dumpsys","media.audio_flinger"});
            return QRegularExpression("\\byes\\s+" + pid +
                "/\\s*\\d+\\s+(?:\\S+\\s+){10}(\\S+)").match(tracks).captured(1);
        };
        if (mStage == 0 && first->isVisible()) {
            if (bar->width() >= bar->sizeHint().width()
                    && (!bar->rect().contains(bar->tabRect(0))
                        || !bar->rect().contains(bar->tabRect(1)))) {
                fail("Selecting the new tab scrolled a fitting tab out of view");
                return;
            }
            auto* leading = bar->tabButton(0, QTabBar::LeftSide);
            auto* logo = leading->findChild<QLabel*>("KemuTabIcon");
            if (!logo || logo->y() != (leading->height() - logo->height()) / 2 + 1 ||
                    logo->x() + logo->width() > first->x() ||
                    bar->tabRect(0).width() != bar->tabRect(1).width() ||
                    bar->childAt(first->mapTo(bar, first->rect().center())) != first) {
                fail("Incorrect tab icon or speaker placement"); return;
            }
            tone("silence");
        } else if (mStage == 1 && !first->isVisible()) {
            mChecks.append("Silent PCM hid the first speaker while the second kept playing");
            tone("tone");
        } else if (mStage == 2 && first->isVisible()) {
            first->click();
        } else if (mStage == 3 && first->isChecked()) {
            const auto snapshot = adb({"shell","dumpsys","activity","service",
                "com.android.emulator.multidisplay/.MultiDisplayService"});
            if (!snapshot.contains("\"muted\"")) return;
            for (auto* speaker : {first, second}) {
                const bool wasHovered = speaker->underMouse();
                auto hasBackground = [speaker](bool hovered) {
                    speaker->setAttribute(Qt::WA_UnderMouse, hovered);
                    const auto image = speaker->grab().toImage();
                    // The left edge of the circle is outside the speaker glyph.
                    return image.pixel(qRound(image.devicePixelRatio()), image.height() / 2)
                            != image.pixel(0, 0);
                };
                const bool idle = hasBackground(false);
                const bool hover = hasBackground(true);
                speaker->setAttribute(Qt::WA_UnderMouse, wasHovered);
                if (idle || !hover || !speaker->toolTip().isEmpty()) {
                    fail("Speaker background or tooltip did not match its hover state"); return;
                }
            }
            mChecks.append("Both speakers showed a hover-only background and no tooltip");
            auto* home = mRoot->findChild<QToolButton*>("KemuDesktopTab");
            home->parentWidget()->grab(home->geometry().united(bar->geometry())).save(
                    qEnvironmentVariable("KEMU_GUI_TEST_RESULTS") + ".png");
            tone("replace");
        } else if (mStage == 4 && first->isChecked() && mElapsed.elapsed() > 1300) {
            const auto other = bar->tabData(1).toJsonObject().value("package").toString();
            const auto otherGain = gain(other);
            if (gain(pkg) != "-inf" || otherGain.isEmpty() || otherGain == "-inf") return;
            mChecks.append("Replacement track was silent in the mixer while the foreground track kept its gain");
            tone("silence");
        } else if (mStage == 5 && !first->isVisible()) {
            tone("tone");
        } else if (mStage == 6 && first->isVisible() && first->isChecked()) {
            mChecks.append("Muted tab hid on silence and restored its muted button when samples resumed");
            first->click();
        } else if (mStage == 7 && !first->isChecked() && first->isVisible() && mElapsed.elapsed() > 800) {
            const auto snapshot = adb({"shell","dumpsys","activity","service",
                "com.android.emulator.multidisplay/.MultiDisplayService"});
            const auto restored = gain(pkg);
            if (snapshot.contains("\"muted\"") || restored.isEmpty() || restored == "-inf") return;
            mChecks.append("Unmuting the background tab restored its detected signal and kept focus");
            mTimer.stop();
            save();
            return;
        } else return;
        advance();
    }
    void checkTabSwitch() {
        if (mElapsed.elapsed() > (mStage == 0 ? 180000 : 10000)) {
            fail("Timed out waiting for app tab focus and input");
            return;
        }
        auto* bar = mRoot->findChild<QTabBar*>("KemuAppTabs");
        auto* desktop = mRoot->findChild<QToolButton*>("KemuDesktopTab");
        if (!bar || !bar->isVisible() || !desktop) return;
        if (mStage == 0 && bar->count() == 1 && !desktop->isChecked() &&
                !mSingleAppChecked && bar->tabData(0).toJsonObject().value("package") != mSwitchApp) {
            if (!reachableDesktop(desktop)) { fail("Home disappeared after the first app launch"); return; }
            gfxstream_display_view firstView{};
            if (!getPresentationView(&firstView) || !firstView.colorBuffer || ++mStable % 15) return;
            const auto package = bar->tabData(0).toJsonObject().value("package").toString();
            const auto windows = adb({"shell", "dumpsys", "window"});
            if (!QRegularExpression("mCurrentFocus=[^\\n]*" + QRegularExpression::escape(package) + "/")
                    .match(windows).hasMatch()) return;
            auto* title = desktop->parentWidget();
            title->grab(desktop->geometry().united(bar->geometry())).save(
                    qEnvironmentVariable("KEMU_GUI_TEST_RESULTS") + ".first-app.png");
            desktop->click();
            mSingleAppChecked = true;
            return;
        }
        if (mStage == 0 && mSingleAppChecked && !mDesktopConfirmed && desktop->isChecked()) {
            gfxstream_display_view desktopView{};
            if (!getPresentationView(&desktopView) || desktopView.slot != 0 || ++mStable % 10) return;
            const auto windows = adb({"shell","dumpsys","window"});
            if (!windows.contains("mTopFocusedDisplayId=0") ||
                    !QRegularExpression("mCurrentFocus=[^\\n]*launcher").match(windows).hasMatch()) return;
            mDesktopConfirmed = true;
            mChecks.append("The first game kept Home visible and clicking it returned to the launcher");
        }
        if (mStage == 9) {
            if (bar->count() == 1 && !desktop->isChecked() && bar->currentIndex() == 0 &&
                    bar->tabData(0).toJsonObject().value("package") == mSwitchApp) {
                mChecks.append("Hover close removed the background tab and kept the game selected");
                mTimer.stop();
                save();
            }
            return;
        }
        if (bar->count() != 2 || !mDesktopConfirmed || desktop->isChecked()) return;
        const int game = bar->tabData(0).toJsonObject().value("package") == mSwitchApp ? 0 : 1;
        const int other = 1 - game;
        const int expected = mStage % 2 == 0 ? game : other;
        const auto tab = bar->tabData(expected).toJsonObject();
        if (bar->tabData(game).toJsonObject().value("package") != mSwitchApp
                || bar->currentIndex() != expected) return;
        gfxstream_display_view view{};
        if (!getPresentationView(&view) || !view.colorBuffer || view.slot != tab.value("slot").toInt()) return;
        if (mStage == 0 && ++mStable < 180) return;
        const auto dump = adb({"shell","dumpsys","activity","service",
                "com.android.emulator.multidisplay/.MultiDisplayService"});
        const auto snapshot = QJsonDocument::fromJson(dump.section("Host snapshot: ", 1)
                .section('\n', 0, 0).trimmed().toUtf8()).object();
        if (snapshot.value("active").toInt(-1) != view.slot ||
                snapshot.value("input").toInt(-1) != view.slot) return;
        if (mStage == 0) {
            if (bar->drawBase() || !desktop->text().isEmpty() || desktop->icon().isNull()
                    || !bar->tabToolTip(game).isEmpty() || !bar->tabToolTip(other).isEmpty()) {
                fail("Unexpected tab base, desktop label or app tooltip");
                return;
            }
            const QRect home = desktop->geometry();
            if (home.width() > home.height() + 4) {
                fail("Desktop icon has excess horizontal space");
                return;
            }
            if (bar->tabRect(game).width() != bar->tabRect(other).width() ||
                    bar->iconSize().height() >= desktop->iconSize().height()) {
                fail("App tabs have unequal widths or oversized icons");
                return;
            }
            for (int i = 0; i < bar->count(); ++i) {
                const auto token = bar->tabData(i).toJsonObject().value("token").toString();
                auto* leading = bar->tabButton(i, QTabBar::LeftSide);
                auto* logo = leading->findChild<QLabel*>("KemuTabIcon");
                QPixmap expected;
                if (!logo || logo->y() != (leading->height() - logo->height()) / 2 + 1 ||
                        !expected.loadFromData(QByteArray::fromStdString(tabIconPng(token.toStdString())), "PNG")
                        || logo->pixmap().toImage() != QIcon(expected).pixmap(QSize(64, 64)).toImage()) {
                    fail("App tab icon does not match the guest launcher icon");
                    return;
                }
            }
            mChecks.append(QJsonObject{{"homeWidth",home.width()}, {"homeHeight",home.height()},
                                       {"appWidth",bar->tabRect(game).width()}, {"appIconHeight",bar->iconSize().height()},
                                       {"verifiedAppIcons",bar->count()}});
            const int maxWidth = bar->maximumWidth();
            bar->setMaximumWidth(80);
            bar->parentWidget()->layout()->activate();
            bool overflow = false;
            for (auto* arrow : bar->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly))
                if (arrow->isVisible()) overflow = true;
            const bool pinned = desktop->geometry() == home && reachableDesktop(desktop);
            bar->setMaximumWidth(maxWidth);
            bar->parentWidget()->layout()->activate();
            if (!overflow || !pinned) { fail("Home moved or disappeared when the app tabs overflowed"); return; }
            mChecks.append("Home stayed pinned while the app strip displayed its scroll controls");
            QHoverEvent leave(QEvent::HoverLeave, QPoint(-1, -1), QPoint(-1, -1), QPoint());
            QApplication::sendEvent(bar, &leave);
            auto* gameClose = closeButton(bar, game);
            auto* otherClose = closeButton(bar, other);
            if (!gameClose || !otherClose || gameClose->isVisible() || otherClose->isVisible()) {
                fail("Close control visible before hover");
                return;
            }
            const auto before = bar->tabRect(game);
            hoverTab(bar, game);
            if (!gameClose->isVisible() || otherClose->isVisible() || bar->tabRect(game) != before) {
                fail("Hover did not expose exactly one close control in place");
                return;
            }
            desktop->parentWidget()->grab(home.united(bar->geometry())).save(
                    qEnvironmentVariable("KEMU_GUI_TEST_RESULTS") + ".png");
            QApplication::sendEvent(bar, &leave);
            mChecks.append("Launcher selected the game; icon-only desktop and hover-only close verified");
        } else {
            mChecks.append(QJsonObject{{"selected",bar->tabText(expected)}, {"inputReadyMs",mElapsed.elapsed()}});
        }
        if (mStage == 8) {
            hoverTab(bar, other);
            auto* close = closeButton(bar, other);
            if (!close || !close->isVisible()) {
                fail("Background tab close control unavailable on hover");
                return;
            }
            advance();
            close->click();
            return;
        }
        const int target = expected == game ? other : game;
        const QPoint pos = bar->tabRect(target).center();
        advance();
        QMouseEvent press(QEvent::MouseButtonPress, pos, bar->mapToGlobal(pos), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, pos, bar->mapToGlobal(pos), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(bar, &press);
        QApplication::sendEvent(bar, &release);
        if (!getPresentationView(&view) || !view.colorBuffer ||
                view.slot != bar->tabData(target).toJsonObject().value("slot").toInt()) {
            fail("Tab click did not immediately select the target's existing color buffer");
        }
    }
    void checkTabPointerInput() {
        mTimer.setInterval(250);
        auto* bar = mRoot->findChild<QTabBar*>("KemuAppTabs");
        if (mElapsed.elapsed() > (mStage == 0 ? 120000 : 10000)) {
            fail("Timed out checking tab pointer input");
            return;
        }
        if (mStage == 2) {
            if (mElapsed.elapsed() < 750) return;
            const auto focus = adb({"shell", "dumpsys window | grep -m1 mTopFocusedDisplayId="});
            if (focus.trimmed() != "mTopFocusedDisplayId=0") return;
            mDisplay->handleMouseWheelEvent(-120, Qt::Vertical);
            advance();
            return;
        }
        if (mStage == 3) {
            if (mWheelEvents.state() != QProcess::NotRunning) return;
            const auto events = mWheelEvents.readAllStandardOutput();
            if (events.count("REL_WHEEL ") != 1 ||
                !QRegularExpression("REL_WHEEL +ffffffff").match(QString::fromUtf8(events)).hasMatch()) {
                fail("Expected only the desktop wheel event on the guest input device");
                return;
            }
            mChecks.append("The native wheel reached the desktop only after selecting Home");
            mTimer.stop();
            save();
            return;
        }
        if (!bar || !bar->isVisible() || bar->currentIndex() < 0) return;
        const auto tab = bar->tabData(bar->currentIndex()).toJsonObject();
        const QString pkg = "org.kemu.tests.tabs.wide";
        if (tab.value("package").toString() != pkg) return;
        if (mStage == 0) {
            const auto logs = adb({"logcat", "-d", "-s", "KemuTabProbe:I", "*:S"});
            const QRegularExpression focused(pkg + " display=" + QString::number(tab.value("display").toInt()) +
                                             " .*focus=true");
            if (!focused.match(logs).hasMatch()) return;
            if (qEnvironmentVariableIsSet("KEMU_GUI_TEST_TAB_WHEEL")) {
                mWheelEvents.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
                    a->flags |= CREATE_NO_WINDOW;
                });
                mWheelEvents.start(qEnvironmentVariable("KEMU_TEST_ADB"),
                        {"-s", "emulator-5554", "shell", "timeout", "5", "getevent", "-lt"});
                if (!mWheelEvents.waitForReadyRead(3000)) { fail("Cannot observe the wheel device"); return; }
                mDisplay->handleMouseWheelEvent(120, Qt::Vertical);
                mRoot->findChild<QToolButton*>("KemuDesktopTab")->click();
                mStage = 2;
                mElapsed.restart();
                return;
            }
            auto event = createSkinEvent(kEventPenPress);
            event.u.pen.tracking_id = INT64_C(0x100000001);
            event.u.pen.x = mDisplay->width() / 2;
            event.u.pen.y = mDisplay->height() / 2;
            event.u.pen.pressure = 400;
            mDisplay->queueSkinEvent(event);
            event.type = kEventPenMove;
            event.u.pen.x += 10;
            mDisplay->queueSkinEvent(event);
            // Switching the target must send a pen-up to the original display.
            mDisplay->setTabInput(-1);
            event.type = kEventPenRelease;
            mDisplay->queueSkinEvent(event);
            mDisplay->setTabInput(tab.value("slot").toInt());
            advance();
            return;
        }
        const QString pid = adb({"shell", "pidof", pkg}).trimmed();
        if (pid.isEmpty()) return;
        const auto logs = adb({"logcat", "-d", "--pid", pid, "-s", "KemuTabProbe:I", "*:S"});
        if (!logs.contains(pkg + " touch=0") || !logs.contains(pkg + " touch=1")) return;
        mChecks.append("The selected app received the pen press and release on input target change");
        mTimer.stop();
        save();
    }

    void checkTabRecovery() {
        mTimer.setInterval(300);
        auto* bar = mRoot->findChild<QTabBar*>("KemuAppTabs");
        auto* desktop = mRoot->findChild<QToolButton*>("KemuDesktopTab");
        if (mElapsed.elapsed() > (mStage == 0 ? 180000 : 20000)) {
            fail("Timed out checking desktop input recovery");
            return;
        }
        if (!desktop || !bar) return;
        const QString service = "com.android.emulator.multidisplay/.MultiDisplayService";
        const QString probe = "org.kemu.tests.tabs.wide";
        if (mStage == 0) {
            if (!bar->isVisible() || desktop->isChecked()) return;
            // ActivityManager prints this confirmation but exits nonzero for a successful stop.
            adb({"shell", "am stopservice -n " + service + " 2>&1 | grep -Fx 'Service stopped'"});
        } else if (mStage == 1) {
            if (bar->isVisible() || !desktop->isChecked()) return;
            if (!reachableDesktop(desktop)) { fail("Disconnected Home is disabled"); return; }
            adb({"shell", "am", "start", "-W", "--display", "0", "-n", probe + "/org.kemu.tests.tabs.Probe"});
        } else if (mStage == 2) {
            const QString pid = adb({"shell", "pidof " + probe + " || true"}).trimmed();
            if (pid.isEmpty()) return;
            const auto logs = adb({"logcat", "-d", "--pid", pid, "-s", "KemuTabProbe:I"});
            if (!logs.contains("key=66 action=1")) {
                SkinEvent key{};
                key.u.key.keycode = LINUX_KEY_ENTER;
                key.type = kEventKeyDown;
                mDisplay->queueSkinEvent(key);
                key.type = kEventKeyUp;
                mDisplay->queueSkinEvent(key);
                return;
            }
            mChecks.append("A desktop activity received host keyboard input with the tab service stopped");
            desktop->click();
        } else if (mStage == 3) {
            if (!adb({"shell", "dumpsys activity activities | grep topDisplayFocusedRootTask"}).contains("type=home")) return;
            mChecks.append("Offline Home opened Launcher on the default display");
            adb({"shell", "am", "startservice", "-n", service});
        } else {
            const QString dump = adb({"shell", "dumpsys", "activity", "service", service});
            if (!dump.contains("\"active\":0") || !dump.contains("\"input\":0")) return;
            mChecks.append("Reconnected with desktop focus and input");
            mTimer.stop();
            save();
            return;
        }
        advance();
    }

    bool reachableDesktop(QToolButton* desktop) {
        return desktop->isVisibleTo(mRoot) && desktop->isEnabled() &&
                mRoot->childAt(desktop->mapTo(mRoot, desktop->rect().center())) == desktop;
    }
    static QToolButton* closeButton(QTabBar* bar, int index) {
        auto* space = bar->tabButton(index, QTabBar::RightSide);
        return space ? space->findChild<QToolButton*>() : nullptr;
    }
    static void hoverTab(QTabBar* bar, int index) {
        const QPoint pos = bar->tabRect(index).center();
        QHoverEvent hover(QEvent::HoverMove, pos, bar->mapToGlobal(pos), QPoint(-1, -1));
        QApplication::sendEvent(bar, &hover);
    }
    bool overviewVisible() {
        const auto launcher = adb({"shell", "dumpsys", "activity", "com.android.launcher3"});
        if (!launcher.contains("mCurrentStableState:Overview")) return false;
        const auto window = adb({"shell", "dumpsys", "window"});
        return QRegularExpression("mTopFocusedDisplayId=0\\b").match(window).hasMatch() &&
                QRegularExpression("mCurrentFocus=[^\\n]*com\\.android\\.launcher3/").match(window).hasMatch();
    }
    bool tapOverviewCard() {
        QXmlStreamReader xml(adb({"shell", "-tt", "uiautomator", "dump", "/dev/tty"}));
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            const auto attr = xml.attributes();
            if (attr.value("resource-id") != QStringLiteral("com.android.launcher3:id/snapshot") ||
                    attr.value("content-desc") != mRecentsLabel) continue;
            const auto bounds = QRegularExpression(R"(\[(\d+),(\d+)\]\[(\d+),(\d+)\])")
                    .match(attr.value("bounds").toString());
            if (!bounds.hasMatch()) return false;
            const auto x = (bounds.captured(1).toInt() + bounds.captured(3).toInt()) / 2;
            const auto y = (bounds.captured(2).toInt() + bounds.captured(4).toInt()) / 2;
            adb({"shell", "input", "-d", "0", "tap", QString::number(x), QString::number(y)});
            return true;
        }
        return false;
    }
    EmulatorContainer* mRoot;
    EmulatorQtWindow* mDisplay;
    QProcess mWheelEvents;
    QTimer mTimer;
    QElapsedTimer mElapsed;
    QSize mBefore;
    QJsonArray mChecks;
    QJsonObject mGeometry;
    QString mMutedBefore;
    QString mMuteApp;
    bool mMuteAppReady = false;
    QString mRecentsApp;
    QString mSwitchApp;
    bool mSingleAppChecked = false;
    bool mDesktopConfirmed = false;
    QString mRecentsLabel;
    QJsonObject mRecentsTab;
    int mStage=0, mStable=0, mFirstFit=-1;
    bool mFailed=false;
};
void startLiveGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display) {
    new LiveGuiChecks(root, display);
}
}  // namespace kemu
