#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

class TelemetryChart : public QWidget {
    Q_OBJECT

public:
    explicit TelemetryChart(QWidget* parent = nullptr);

    void addDataPoint(double value);
    void setAccent(const QColor& color);
    void setMaxPoints(int n);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<double> m_data;
    QColor m_accent{QStringLiteral("#D5B56B")};
    int m_maxPoints = 80;
};
