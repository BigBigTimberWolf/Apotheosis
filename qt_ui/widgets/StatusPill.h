#pragma once

#include <QWidget>

class QLabel;

class StatusPill : public QWidget {
    Q_OBJECT

public:
    enum Tone { Neutral, Success, Warning, Danger };

    explicit StatusPill(QWidget* parent = nullptr);

    void setStatus(const QString& text, Tone tone = Neutral);

private:
    QLabel* m_dot{};
    QLabel* m_text{};
};
