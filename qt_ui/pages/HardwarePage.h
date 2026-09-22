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
    void reconnectMouseOnly();
    void reconnectKbdOnly();
    void loadFieldsFromConfig();

private:
    void syncConfigToRuntime();
    // 键盘硬件卡片只对 MAKCU(hybrid) 与 MAKCUNEW 有意义, KMBOXNET 下隐藏。
    void updateKbdCardVisibility(int methodIndex);

    // 重新枚举系统串口, 灌进三个串口下拉。
    // 保留当前已配置值: 即使那个口现在没插, 也会作为一项加进去(标注"未检测到"),
    // 避免打开界面时把用户配置冲掉。
    void refreshPortLists();

    // 下拉的实际取值 —— 用 itemData 承载真值, 因为显示文本可能带"(未检测到)"后缀。
    static QString comboText(const QComboBox* box);
    static int comboNumber(const QComboBox* box);

    QComboBox* m_inputMethodCombo{};
    QStackedWidget* m_deviceStack{};
    QLabel* m_statusDot{};
    QLabel* m_statusText{};
    QPushButton* m_connectBtn{};
    QTimer* m_statusTimer{};
    QPushButton* m_refreshPortsBtn{};

    // 串口 / 波特率全部改成【下拉选择】, 避免手打串口名出错(历史上出过 COM0 这种占位值)。
    QComboBox* m_makcuPort{};
    QComboBox* m_makcuBaud{};
    QComboBox* m_makcuNewPort{};
    QComboBox* m_makcuNewBaud{};
    // 键盘硬件(第二台, 可选)。不勾选时键盘注入与自动急停不可用, 鼠标不受影响。
    //
    // ★ 这套控件由 MAKCU 与 MAKCUNEW 两种方式【共用】(配置项也是同一个
    //   makcu_new_port_kbd), 因此只创建一次、放在 m_kbdCard 里, 不放进
    //   deviceStack 的任一页 —— 否则成员指针会被后建的那套覆盖。
    QCheckBox* m_kbdUnitEnabled{};
    QWidget* m_kbdUnitPanel{};
    QComboBox* m_makcuNewPortKbd{};
    QComboBox* m_makcuNewBaudKbd{};
    QPushButton* m_connectKbdBtn{};
    QLabel* m_kbdStatusDot{};
    QLabel* m_kbdStatusText{};
    CardWidget* m_kbdCard{};

    QLineEdit* m_kmboxNetIp{};
    QLineEdit* m_kmboxNetPort{};
    QLineEdit* m_kmboxNetUuid{};

};
