#include "widgets/StatusPill.h"

#include <QHBoxLayout>
#include <QLabel>

StatusPill::StatusPill(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 6, 0);
    row->setSpacing(7);

    m_dot = new QLabel(this);
    m_dot->setFixedSize(7, 7);

    m_text = new QLabel(this);
    m_text->setStyleSheet("background:transparent;");

    row->addWidget(m_dot);
    row->addWidget(m_text);

    setStatus(QString(), Neutral);
}

void StatusPill::setStatus(const QString& text, Tone tone) {
    QString dot, fg;
    switch (tone) {
        case Success: dot = "#22C55E"; fg = "#53C583"; break;
        case Warning: dot = "#F5A623"; fg = "#E9BD69"; break;
        case Danger:  dot = "#E5484D"; fg = "#EF7A71"; break;
        case Neutral:
        default:      dot = "#B0B0B8"; fg = "#ABA697"; break;
    }

    setStyleSheet(QStringLiteral("background:transparent;"));
    m_dot->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(dot));
    m_text->setStyleSheet(QStringLiteral("background:transparent; color:%1; font-size:12px;").arg(fg));
    m_text->setText(text);
}
