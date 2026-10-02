#pragma once

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

namespace ApotheosisTheme {

inline void apply(QApplication& app) {
    app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setWindowIcon(QIcon(QStringLiteral(":/assets/apotheosis_logo.png")));
    QPalette pal;
    pal.setColor(QPalette::Window, QColor("#101012"));
    pal.setColor(QPalette::WindowText, QColor("#F0EDE6"));
    pal.setColor(QPalette::Base, QColor("#19191C"));
    pal.setColor(QPalette::AlternateBase, QColor("#1D1D20"));
    pal.setColor(QPalette::Text, QColor("#F0EDE6"));
    pal.setColor(QPalette::Button, QColor("#222226"));
    pal.setColor(QPalette::ButtonText, QColor("#F0EDE6"));
    pal.setColor(QPalette::ToolTipBase, QColor("#222226"));
    pal.setColor(QPalette::ToolTipText, QColor("#F0EDE6"));
    pal.setColor(QPalette::PlaceholderText, QColor("#8E887A"));
    pal.setColor(QPalette::Highlight, QColor("#D5B56B"));
    pal.setColor(QPalette::HighlightedText, QColor("#101012"));
    pal.setColor(QPalette::Light, QColor("#514C40"));
    pal.setColor(QPalette::Midlight, QColor("#35332D"));
    pal.setColor(QPalette::Mid, QColor("#302E28"));
    pal.setColor(QPalette::Dark, QColor("#0C0C0E"));
    pal.setColor(QPalette::Shadow, QColor("#080809"));
    pal.setColor(QPalette::Link, QColor("#D5B56B"));
    pal.setColor(QPalette::LinkVisited, QColor("#C2AD7C"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        pal.setColor(QPalette::Disabled, role, QColor("#6D685F"));
    app.setPalette(pal);

    QFont font;
    font.setFamilies({QStringLiteral("Segoe UI Variable"),
                      QStringLiteral("Microsoft YaHei UI"),
                      QStringLiteral("Segoe UI")});
    font.setPixelSize(13);
    app.setFont(font);
}

} // namespace ApotheosisTheme
