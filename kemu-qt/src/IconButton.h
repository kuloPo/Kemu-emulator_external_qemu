#pragma once
#include <QAbstractButton>

enum class Icon { VolumeUp, VolumeDown, Mute, Back, Home, Recents, Settings, Minimize, Maximize, Restore, Close };
class IconButton : public QAbstractButton {
    Q_OBJECT
public:
    IconButton(Icon icon, const QString& label, QWidget* parent = nullptr);
    void setIconType(Icon icon) { m_icon = icon; update(); }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    Icon m_icon;
};
