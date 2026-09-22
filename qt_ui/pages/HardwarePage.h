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
class CardWidget;

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
    // 键盘硬件卡片只对 MAKCU(hybrid) 与 MAKCUNEW 有意义, KMBOXNET 下隐藏。
    void updateKbdCardVisibility(int methodIndex);

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
    //
    // ★ 这套控件由 MAKCU 与 MAKCUNEW 两种方式【共用】(配置项也是同一个
    //   makcu_new_port_kbd), 因此只创建一次、放在 m_kbdCard 里, 不放进
    //   deviceStack 的任一页 —— 否则成员指针会被后建的那套覆盖。
    QCheckBox* m_kbdUnitEnabled{};
    QWidget* m_kbdUnitPanel{};
    QLineEdit* m_makcuNewPortKbd{};
    QSpinBox* m_makcuNewBaudKbd{};
    CardWidget* m_kbdCard{};

    QLineEdit* m_kmboxNetIp{};
    QLineEdit* m_kmboxNetPort{};
    QLineEdit* m_kmboxNetUuid{};

};
