#include "widgets/ToggleSwitch.h"

#include <QPainter>
#include <QPropertyAnimation>
#include <QSizePolicy>

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(sizeHint());

    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        if (m_animation) {
            m_animation->stop();
            m_animation->deleteLater();
        }
        m_animation = new QPropertyAnimation(this, "pos", this);
        m_animation->setDuration(150);
        m_animation->setStartValue(m_pos);
        m_animation->setEndValue(on ? 1.0 : 0.0);
        m_animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_animation, &QPropertyAnimation::finished, this, [this] {
            m_animation->deleteLater();
            m_animation = nullptr;
        });
        m_animation->start();
    });
}

QSize ToggleSwitch::sizeHint() const {
    return {46, 26};
}

void ToggleSwitch::checkStateSet() {
    QAbstractButton::checkStateSet();
    if (signalsBlocked()) {
        if (m_animation) {
            m_animation->stop();
            m_animation->deleteLater();
            m_animation = nullptr;
        }
        m_pos = isChecked() ? 1.0 : 0.0;
        update();
    }
}

void ToggleSwitch::setPos(qreal pos) {
    m_pos = pos;
    update();
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    const qreal w = width();
    const qreal h = height();
    const qreal r = h / 2.0;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);

    const QColor offTrack = isEnabled() ? QColor("#44413A") : QColor("#292825");
    const QColor onTrack = isEnabled() ? QColor("#D5B56B") : QColor("#655838");
    p.setBrush(offTrack);
    p.drawRoundedRect(QRectF(0, 0, w, h), r, r);

    if (m_pos > 0.0) {
        p.setOpacity(m_pos);
        p.setBrush(onTrack);
        p.drawRoundedRect(QRectF(0, 0, w, h), r, r);
        p.setOpacity(1.0);
    }

    const qreal margin = 3.0;
    const qreal knobD = h - 2 * margin;
    const qreal x = margin + m_pos * (w - knobD - 2 * margin);

    p.setBrush(QColor("#F0EDE6"));
    p.setPen(QPen(QColor(0, 0, 0, 28), 0.5));
    p.drawEllipse(QRectF(x, margin, knobD, knobD));

    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(213, 181, 107, 160), 2));
        p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), r - 1, r - 1);
    }
}
