#pragma once

#include <QString>

class QWidget;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class ToggleSwitch;

namespace FormKit {

QWidget* sliderRow(const QString& label, int min, int max, int value,
                   QSlider*& sliderOut, QSpinBox*& spinOut,
                   const QString& suffix = QString());

QWidget* sliderRowD(const QString& label, double min, double max, double value,
                    double step, int decimals,
                    QSlider*& sliderOut, QDoubleSpinBox*& spinOut,
                    const QString& suffix = QString());

QWidget* fieldRow(const QString& label, QWidget* control);

QWidget* toggleRow(const QString& label, bool checked, ToggleSwitch*& toggleOut);

}
