#pragma once
#include "../launcher/PerformanceConfig.h"
#include "UiFont.h"
#include "DetachedProcess.h"
#include <QButtonGroup>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <functional>
#include <initializer_list>

namespace kemu {
class PerformanceRestartPrompt final : public UiMessageBox {
public:
    using UiMessageBox::UiMessageBox;
protected:
    void showEvent(QShowEvent* event) override {
        UiMessageBox::showEvent(event);
        auto* buttons=findChild<QDialogButtonBox*>();
        auto* row=buttons ? qobject_cast<QBoxLayout*>(buttons->layout()) : nullptr;
        if (!row) return;
        QPushButton *later=nullptr, *now=nullptr;
        for (auto* button:buttons->buttons()) {
            if (buttons->buttonRole(button)==QDialogButtonBox::RejectRole) later=qobject_cast<QPushButton*>(button);
            if (buttons->buttonRole(button)==QDialogButtonBox::AcceptRole) now=qobject_cast<QPushButton*>(button);
        }
        if (!later || !now) return;
        row->removeWidget(later); row->removeWidget(now);
        row->addStretch(); row->addWidget(later); row->addWidget(now);
        layout()->setAlignment(buttons,Qt::AlignRight);
        QWidget::setTabOrder(later,now);
        layout()->activate();
    }
};
class PerformancePage : public QWidget {
public:
    using Restart = std::function<bool(const QString&)>;
    explicit PerformancePage(QWidget* parent, Restart restart = {})
        : QWidget(parent), mRestart(std::move(restart)) {
        auto avd = qEnvironmentVariable("ANDROID_AVD_HOME");
        if (avd.isEmpty()) avd = qEnvironmentVariable("LOCALAPPDATA") + "/Kemu/Kemu/avd";
        mPath = avd + "/performance.ini";
        const auto config = PerformanceConfig::load(std::filesystem::path(mPath.toStdWString()));
        mCustomWidth = config.customWidth; mCustomHeight = config.customHeight; mCustomFps = config.customFps;
        auto* form = new QFormLayout(this);
        form->setVerticalSpacing(16);
        form->setHorizontalSpacing(18);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
        setStyleSheet(
            "QRadioButton {background:#232d3a;border:1px solid #465468;border-radius:8px;padding:8px 10px;spacing:8px;}"
            "QRadioButton:hover {background:#2c3b4e;border-color:#708dae;}"
            "QRadioButton:checked {background:#29456b;border-color:#69a5f5;color:#d6e8ff;}"
            "QRadioButton:focus {border:1px solid #b8d8ff;}"
            "QRadioButton::indicator {width:12px;height:12px;border:1px solid #8d9caf;border-radius:7px;background:#151b24;}"
            "QRadioButton::indicator:checked {background:#8fc3ff;border-color:#b8d8ff;}"
            "QSpinBox {background:#29323e;color:#edf1f6;border:1px solid #465468;border-radius:6px;padding:7px;}"
            "QSlider::groove:horizontal {height:6px;background:#465468;border-radius:3px;}"
            "QSlider::sub-page:horizontal {background:#508ee0;border-radius:3px;}"
            "QSlider::handle:horizontal {width:16px;margin:-5px 0;background:#8fc3ff;border-radius:8px;}"
            "QSlider:focus {border:1px solid #8fc3ff;border-radius:4px;}");
        auto spin = [&](const char* name, int low, int high, int value, bool expand = false) {
            auto* box = new QSpinBox(this); box->setObjectName(name);
            box->setRange(low, high); box->setValue(value);
            if (expand) box->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
            else box->setFixedWidth(100);
            box->setKeyboardTracking(false); box->setButtonSymbols(QAbstractSpinBox::NoButtons);
            return box;
        };
        auto row = [&](QWidget* widget, const char* text) {
            if (!text) { form->setWidget(form->rowCount(),QFormLayout::FieldRole,widget); return; }
            auto* label = new QLabel(this); label->setBuddy(widget);
            mLabels.push_back({label,text}); form->addRow(label, widget);
        };
        auto choices = [&](const char* name, std::initializer_list<int> ids, int selected, const char* text, QButtonGroup*& group) {
            auto* widget = new QWidget(this); auto* grid = new QGridLayout(widget);
            grid->setContentsMargins(0,0,0,0); grid->setSpacing(8);
            grid->setColumnStretch(0,1); grid->setColumnStretch(1,1);
            group = new QButtonGroup(this); group->setObjectName(name);
            int slot = 0;
            for (const int id : ids) {
                auto* button = new QRadioButton(widget);
                button->setObjectName(QString(name)+QString::number(id));
                button->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
                group->addButton(button,id); grid->addWidget(button,slot/2,slot%2); ++slot;
            }
            group->button(selected)->setChecked(true);
            row(widget,text); return grid;
        };
        auto* resolutionGrid = choices("KemuResolution",{0,1,2,3,4},config.resolution,QT_TRANSLATE_NOOP("Kemu","Resolution"),mResolution);
        mDimensions = new QWidget(this); auto* dimensions = new QHBoxLayout(mDimensions);
        dimensions->setContentsMargins(0,0,0,0); dimensions->setSpacing(10);
        mWidth = spin("KemuWidth",320,7680,config.width,true); mHeight = spin("KemuHeight",320,7680,config.height,true);
        dimensions->addWidget(mWidth,1); dimensions->addWidget(new QLabel("x",this)); dimensions->addWidget(mHeight,1);
        resolutionGrid->addWidget(mDimensions,2,1);
        choices("KemuOrientation",{0,1},config.portrait,nullptr,mOrientation);
        auto* fpsGrid = choices("KemuFpsMode",{0,1,4,2,3},config.fpsMode,QT_TRANSLATE_NOOP("Kemu","FPS"),mFpsMode);
        mFps = spin("KemuFps",1,1000,config.fps,true); fpsGrid->addWidget(mFps,2,1);
        auto resource = [&](const char* name, const char* text, int minimum, int maximum, int value, QSpinBox*& input) {
            auto* group = new QWidget(this); auto* line = new QHBoxLayout(group); line->setContentsMargins(0,0,0,0); line->setSpacing(12);
            auto* slider = new QSlider(Qt::Horizontal,group); slider->setObjectName(QString(name)+"Slider"); slider->setRange(minimum,maximum); slider->setValue(value);
            input = spin(name,minimum,maximum,value);
            connect(slider,&QSlider::valueChanged,input,&QSpinBox::setValue);
            connect(input,QOverload<int>::of(&QSpinBox::valueChanged),slider,&QSlider::setValue);
            line->addWidget(slider,1); line->addWidget(input); row(group,text);
        };
        resource("KemuCpu",QT_TRANSLATE_NOOP("Kemu","CPU"),1,hostCpuCount(),config.cores,mCores);
        resource("KemuRam",QT_TRANSLATE_NOOP("Kemu","RAM"),4,(std::max)(4,hostRamGb()),config.ramGb,mRam); mRam->setSuffix(" G");
        auto* storage = new QWidget(this); auto* storageRow = new QGridLayout(storage);
        storageRow->setContentsMargins(0,0,0,0); storageRow->setSpacing(12);
        mStorage = new QLabel(storage); mStorage->setObjectName("KemuStorageUsage");
        mExpand = new QPushButton(storage); mExpand->setObjectName("KemuExpandDisk"); mExpand->setEnabled(false);
        mDiskPath = new QLabel(storage); mDiskPath->setObjectName("KemuStoragePath");
        mDiskPath->setTextFormat(Qt::PlainText);
        mDiskPath->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        mDiskPath->installEventFilter(this);
        mMove = new QPushButton(storage); mMove->setObjectName("KemuMoveDisk"); mMove->setEnabled(false);
        for (auto* button : {mExpand,mMove}) button->setStyleSheet("padding:0px 5px;");
        mRam->installEventFilter(this);
        storageRow->setColumnStretch(0,1);
        storageRow->addWidget(mStorage,0,0); storageRow->addWidget(mExpand,0,1);
        storageRow->addWidget(mDiskPath,1,0); storageRow->addWidget(mMove,1,1);
        row(storage,QT_TRANSLATE_NOOP("Kemu","Storage"));
        connect(mExpand,&QPushButton::clicked,this,[this] { if (expandStorage) expandStorage(); });
        connect(mMove,&QPushButton::clicked,this,[this] { if (moveStorage) moveStorage(); });
        connect(mResolution,&QButtonGroup::idClicked,this,[this](int id) {
            if (id == 4) { const int width=mCustomWidth, height=mCustomHeight; mWidth->setValue(width); mHeight->setValue(height); }
            updateResolution();
        });
        connect(mOrientation,&QButtonGroup::idClicked,this,[this] { updateResolution(); updateResolutionLabels(); });
        connect(mFpsMode,&QButtonGroup::idClicked,this,[this](int id) {
            if (id == 3) mFps->setValue(mCustomFps);
            updateFps();
        });
        auto rememberDimensions = [this] {
            if (mResolution->checkedId() == 4) { mCustomWidth=mWidth->value(); mCustomHeight=mHeight->value(); }
        };
        connect(mWidth,QOverload<int>::of(&QSpinBox::valueChanged),this,rememberDimensions);
        connect(mHeight,QOverload<int>::of(&QSpinBox::valueChanged),this,rememberDimensions);
        connect(mFps,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int value) { if (mFpsMode->checkedId()==3) mCustomFps=value; });
        auto* refresh = new QTimer(this); refresh->setInterval(1000);
        connect(refresh,&QTimer::timeout,this,[this] { if (isVisible()) updateFps(); }); refresh->start();
        updateResolution(); retranslate(); mSaved = contents();
    }
    void retranslate() {
        for (const auto& label : mLabels) label.first->setText(QCoreApplication::translate("Kemu",label.second));
        updateResolutionLabels();
        mResolution->button(4)->setText(QCoreApplication::translate("Kemu","Custom"));
        mFpsMode->button(0)->setText("30 FPS"); mFpsMode->button(1)->setText("60 FPS"); mFpsMode->button(4)->setText("120 FPS");
        updateFps(); mFpsMode->button(3)->setText(QCoreApplication::translate("Kemu","Custom"));
        mOrientation->button(0)->setText(QCoreApplication::translate("Kemu","Landscape"));
        mOrientation->button(1)->setText(QCoreApplication::translate("Kemu","Portrait"));
        mExpand->setText(QCoreApplication::translate("Kemu","Expand disk"));
        mMove->setText(QCoreApplication::translate("Kemu","Move"));
    }
    void setStorage(const QString& text, bool enabled, const QString& path, bool movable) {
        mStorage->setText(text); mExpand->setEnabled(enabled);
        mDiskPath->setToolTip(QDir::toNativeSeparators(path));
        mDiskPath->setAccessibleName(QDir::toNativeSeparators(path));
        updateDiskPath(); mMove->setEnabled(movable);
    }
    std::function<void()> expandStorage, moveStorage;
    bool save(QDialog* dialog) {
        for (auto* input : {mWidth,mHeight,mFps,mCores,mRam}) input->interpretText();
        updateResolution(); updateFps();
        const auto data = contents();
        if (data == mSaved) return true;
        QSaveFile file(mPath);
        if (!QDir().mkpath(QFileInfo(mPath).absolutePath()) || !file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            UiMessageBox warning(QMessageBox::Warning,QCoreApplication::translate("Kemu","Performance"),QCoreApplication::translate("Kemu","Could not save performance settings. Please try again."),QMessageBox::Ok,dialog);
            warning.exec(); dialog->show(); return false;
        }
        mSaved = data;
        return true;
    }
    void closed(QDialog* dialog, QWidget* window, const QString& root, bool canRestart = true) {
        const auto previous = mSaved;
        if (!save(dialog) || mSaved == previous) return;
        PerformanceRestartPrompt prompt(QMessageBox::Information,QCoreApplication::translate("Kemu","Restart required"),QCoreApplication::translate("Kemu","Performance settings will take effect after restarting Kemu."),QMessageBox::NoButton,dialog);
        prompt.setObjectName("KemuPerformanceRestart");
        auto* now = prompt.addButton(QCoreApplication::translate("Kemu","Restart now"),QMessageBox::AcceptRole);
        auto* later = prompt.addButton(QCoreApplication::translate("Kemu","Restart later"),QMessageBox::RejectRole);
        now->setEnabled(canRestart);
        prompt.setDefaultButton(later); prompt.setEscapeButton(later); prompt.exec();
        if (prompt.clickedButton() == now) {
            const bool started = mRestart ? mRestart(root) : startDetachedProcess(root + "/Kemu.exe", {"--restart-pid",QString::number(GetCurrentProcessId())},root);
            if (started) window->close();
            else {
                UiMessageBox warning(QMessageBox::Warning,QCoreApplication::translate("Kemu","Restart required"),QCoreApplication::translate("Kemu","Could not restart Kemu. Please close and reopen it manually."),QMessageBox::Ok,dialog);
                warning.exec();
            }
        }
    }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == mRam && event->type() == QEvent::Resize)
            for (auto* button : {mExpand,mMove}) button->setFixedSize(mRam->size());
        if (watched == mDiskPath && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange))
            updateDiskPath();
        return QWidget::eventFilter(watched,event);
    }
private:
    void updateDiskPath() {
        mDiskPath->setText(mDiskPath->fontMetrics().elidedText(mDiskPath->toolTip(),Qt::ElideMiddle,mDiskPath->contentsRect().width()));
    }
    void updateResolutionLabels() {
        for (int i=0;i<4;++i) {
            const auto& size = PerformanceConfig::sizes[i];
            const bool portrait = mOrientation->checkedId()==1;
            mResolution->button(i)->setText(QString("%1x%2").arg(size[portrait ? 1 : 0]).arg(size[portrait ? 0 : 1]));
        }
    }
    void updateResolution() {
        const int preset = mResolution->checkedId(); mDimensions->setVisible(preset==4);
        int width = mWidth->value(), height = mHeight->value();
        if (preset < 4) { width=PerformanceConfig::sizes[preset][0]; height=PerformanceConfig::sizes[preset][1]; }
        if ((mOrientation->checkedId()==1 && width>height) || (mOrientation->checkedId()==0 && width<height)) std::swap(width,height);
        mWidth->setValue(width); mHeight->setValue(height);
    }
    void updateFps() {
        const int mode = mFpsMode->checkedId(); mFps->setVisible(mode==3);
        const auto* display = window()->screen();
        const int rate = display && display->refreshRate()>1 ? std::clamp(qRound(display->refreshRate()),1,1000) : 60;
        mFpsMode->button(2)->setText(QCoreApplication::translate("Kemu","Current screen (%1 FPS)").arg(rate));
        if (mode < 2) mFps->setValue(mode==0 ? 30 : 60);
        else if (mode==2) mFps->setValue(rate);
        else if (mode==4) mFps->setValue(120);
        mDisplayName = display ? display->name() : QString();
    }
    QByteArray contents() const {
        return QString("[performance]\nresolution=%1\nwidth=%2\nheight=%3\nfpsMode=%4\nfps=%5\nportrait=%6\ncores=%7\nramGb=%8\ncustomWidth=%9\ncustomHeight=%10\ncustomFps=%11\ndisplayName=%12\n")
            .arg(mResolution->checkedId()).arg(mWidth->value()).arg(mHeight->value()).arg(mFpsMode->checkedId()).arg(mFps->value()).arg(mOrientation->checkedId()).arg(mCores->value()).arg(mRam->value())
            .arg(mCustomWidth).arg(mCustomHeight).arg(mCustomFps).arg(mFpsMode->checkedId()==2 ? mDisplayName : QString()).toUtf8();
    }
    QString mPath, mDisplayName;
    QByteArray mSaved;
    Restart mRestart;
    QList<QPair<QLabel*,const char*>> mLabels;
    QButtonGroup *mResolution, *mFpsMode, *mOrientation;
    QSpinBox *mWidth, *mHeight, *mFps, *mCores, *mRam;
    QWidget* mDimensions;
    QLabel *mStorage, *mDiskPath;
    QPushButton *mExpand, *mMove;
    int mCustomWidth, mCustomHeight, mCustomFps;
};
}
