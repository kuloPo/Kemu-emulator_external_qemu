#include "IconButton.h"
#include <QPainter>
#include <QPainterPath>
#include <cmath>

IconButton::IconButton(Icon icon, const QString& label, QWidget* parent)
    : QAbstractButton(parent), m_icon(icon) {
    setToolTip(label);
    setAccessibleName(label);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(40, 40);
}

void IconButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool close = m_icon == Icon::Close;
    if (underMouse() || isDown()) {
        const QColor background = close ? QColor(isDown() ? "#bd3947" : "#de4655")
            : QColor(isDown() ? "#344252" : "#29323e");
        p.setPen(Qt::NoPen); p.setBrush(background);
        p.drawRoundedRect(rect().adjusted(3, 3, -3, -3), 6, 6);
    }
    const QColor foreground = !isEnabled() ? QColor("#515966")
        : underMouse() && !close ? QColor("#9bc5ff") : QColor("#d3dae4");
    p.translate(width() / 2.0 - 12, height() / 2.0 - 12);
    p.setPen(QPen(foreground, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    switch (m_icon) {
    case Icon::VolumeUp:
    case Icon::VolumeDown:
    case Icon::Mute: {
        QPainterPath speaker;
        speaker.moveTo(4, 9); speaker.lineTo(8, 9); speaker.lineTo(12, 5);
        speaker.lineTo(12, 19); speaker.lineTo(8, 15); speaker.lineTo(4, 15); speaker.closeSubpath();
        p.drawPath(speaker);
        if (m_icon == Icon::Mute) { p.drawLine(QPointF(16, 9), QPointF(21, 15)); p.drawLine(QPointF(21, 9), QPointF(16, 15)); }
        else {
            p.drawLine(QPointF(16, 12), QPointF(22, 12));
            if (m_icon == Icon::VolumeUp) p.drawLine(QPointF(19, 9), QPointF(19, 15));
        }
        break;
    }
    case Icon::Back: p.drawLine(QPointF(18, 12), QPointF(6, 12)); p.drawPolyline(QPolygonF{QPointF(11, 7), QPointF(6, 12), QPointF(11, 17)}); break;
    case Icon::Home: p.drawEllipse(QRectF(5.5, 5.5, 13, 13)); break;
    case Icon::Recents: p.drawRoundedRect(QRectF(5.5, 5.5, 13, 13), 2, 2); break;
    case Icon::Settings: {
        QPainterPath gear;
        for (int tooth = 0; tooth < 8; ++tooth) {
            for (int corner = 0; corner < 4; ++corner) {
                const double angle = (tooth * 45.0 + corner * 11.25 - 5.625) * 3.141592653589793 / 180.0;
                const double radius = (corner == 1 || corner == 2) ? 9.0 : 7.0;
                QPointF point(12 + radius * std::cos(angle), 12 + radius * std::sin(angle));
                if (!tooth && !corner) gear.moveTo(point); else gear.lineTo(point);
            }
        }
        gear.closeSubpath(); p.drawPath(gear); p.drawEllipse(QRectF(9, 9, 6, 6)); break;
    }
    case Icon::Minimize: p.drawLine(QPointF(7, 12), QPointF(17, 12)); break;
    case Icon::Maximize: p.drawRect(QRectF(7, 7, 10, 10)); break;
    case Icon::Restore: p.drawRect(QRectF(6, 9, 9, 9)); p.drawPolyline(QPolygonF{QPointF(9, 6), QPointF(18, 6), QPointF(18, 15)}); break;
    case Icon::Close: p.drawLine(QPointF(7, 7), QPointF(17, 17)); p.drawLine(QPointF(17, 7), QPointF(7, 17)); break;
    }
}
