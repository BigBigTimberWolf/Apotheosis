#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;

class HardwarePage : public QWidget {
    Q_OBJECT

public:
    explicit HardwarePage(QWidget* parent = nullptr);

private slots:
    void onInputMethodChanged(int index);
    void refreshStatus();
    void reconnectDevice();
    void loadFieldsFromConfig();

private:
    QComboBox* m_inputMethodCombo{};
    QStackedWidget* m_deviceStack{};
    QLabel* m_statusDot{};
    QLabel* m_statusText{};
    QPushButton* m_connectBtn{};
    QTimer* m_statusTimer{};

    QLineEdit* m_makcuPort{};
    QSpinBox* m_makcuBaud{};
    QLineEdit* m_makcuNewPort{};
    QSpinBox* m_makcuNewBaud{};
    // 键盘硬件(第二台, 可选)。不勾选时键盘注入与自动急停不可用, 鼠标不受影响。
    QCheckBox* m_kbdUnitEnabled{};
    QWidget* m_kbdUnitPanel{};
    QLineEdit* m_makcuNewPortKbd{};
    QSpinBox* m_makcuNewBaudKbd{};

    QLineEdit* m_kmboxNetIp{};
    QLineEdit* m_kmboxNetPort{};
    QLineEdit* m_kmboxNetUuid{};

};
