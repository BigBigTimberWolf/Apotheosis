#include "pages/HardwarePage.h"

#include "Apotheosis.h"
#include "runtime/config_snapshot.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include "config/config_bridge.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <mutex>

#include "mouse/Makcu.h"
#include "mouse/MakcuNew.h"
#include "mouse/kmboxNetConnection.h"

namespace {

QString zh(const char* text)
{
    return QString::fromUtf8(text);
}

constexpr const char* kInputMethodIds[] = {"MAKCU", "MAKCUNEW", "KMBOXNET"};
constexpr int kInputMethodCount = 3;

}

HardwarePage::HardwarePage(QWidget* parent)
    : QWidget(parent)
{
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outerLayout->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    auto* inputCard = new CardWidget(zh(u8"输入方式"), QStringLiteral("plug"));
    m_inputMethodCombo = new QComboBox;
    m_inputMethodCombo->addItems({
        QStringLiteral("MAKCU"),
        QStringLiteral("MAKCUNEW"),
        QStringLiteral("KMBOXNET")
    });
    inputCard->contentLayout()->addWidget(
        FormKit::fieldRow(zh(u8"方式"), m_inputMethodCombo));
    layout->addWidget(inputCard);

    auto* statusCard = new CardWidget(zh(u8"连接状态"), QStringLiteral("wifi"));
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    m_statusDot = new QLabel(QString::fromUtf8(u8"●"));
    m_statusDot->setFixedWidth(20);
    statusRow->addWidget(m_statusDot);
    m_statusText = new QLabel;
    m_statusText->setStyleSheet("font-size:13px;");
    statusRow->addWidget(m_statusText, 1);
    m_connectBtn = new QPushButton(zh(u8"连接"));
    m_connectBtn->setFixedHeight(28);
    m_connectBtn->setCursor(Qt::PointingHandCursor);
    statusRow->addWidget(m_connectBtn);
    statusCard->contentLayout()->addLayout(statusRow);
    layout->addWidget(statusCard);

    auto* deviceCard = new CardWidget(zh(u8"设备参数"), QStringLiteral("adjustments"));
    m_deviceStack = new QStackedWidget;

    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0, 0, 0, 0);
        panel->setSpacing(10);
        m_makcuPort = new QLineEdit;
        panel->addWidget(FormKit::fieldRow(zh(u8"串口"), m_makcuPort));
        m_makcuBaud = new QSpinBox;
        m_makcuBaud->setRange(1200, 921600);
        panel->addWidget(FormKit::fieldRow(zh(u8"波特率"), m_makcuBaud));
        m_deviceStack->addWidget(page);
    }

    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0, 0, 0, 0);
        panel->setSpacing(10);

        // ---- 鼠标硬件 ----
        // 位移 / 左右中键 / 滚轮 / 物理按键读取都走这台。
        auto* mouseTitle = new QLabel(zh(u8"鼠标"));
        mouseTitle->setStyleSheet("font-weight:600; font-size:13px;");
        panel->addWidget(mouseTitle);

        m_makcuNewPort = new QLineEdit;
        m_makcuNewPort->setPlaceholderText(QStringLiteral("COM7"));
        panel->addWidget(FormKit::fieldRow(zh(u8"串口"), m_makcuNewPort));
        m_makcuNewBaud = new QSpinBox;
        m_makcuNewBaud->setRange(1200, 6000000);
        panel->addWidget(FormKit::fieldRow(zh(u8"波特率"), m_makcuNewBaud));

        // ---- 键盘硬件(可选) ----
        //
        // 独立的一台硬件: 真实键盘插在它上面。键盘注入(tapKey)与自动急停的
        // 屏蔽命令都只走这台, 因此【不接键盘硬件也不影响鼠标】——
        // 只是键盘注入与自动急停不可用, 鼠标位移/按键/滚轮完全不受影响。
        m_kbdUnitEnabled = new QCheckBox(zh(u8"接入键盘硬件(第二台)"));
        m_kbdUnitEnabled->setToolTip(zh(
            u8"勾选后键盘注入与自动急停走这一台;\n"
            u8"不勾选则键盘注入与自动急停不可用, 鼠标功能不受影响。"));
        panel->addWidget(m_kbdUnitEnabled);

        m_kbdUnitPanel = new QWidget;
        auto* kbdPanel = new QVBoxLayout(m_kbdUnitPanel);
        kbdPanel->setContentsMargins(0, 0, 0, 0);
        kbdPanel->setSpacing(10);

        m_makcuNewPortKbd = new QLineEdit;
        m_makcuNewPortKbd->setPlaceholderText(QStringLiteral("COM9"));
        kbdPanel->addWidget(FormKit::fieldRow(zh(u8"串口"), m_makcuNewPortKbd));
        m_makcuNewBaudKbd = new QSpinBox;
        m_makcuNewBaudKbd->setRange(1200, 6000000);
        kbdPanel->addWidget(FormKit::fieldRow(zh(u8"波特率"), m_makcuNewBaudKbd));

        panel->addWidget(m_kbdUnitPanel);
        m_deviceStack->addWidget(page);
    }

    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0, 0, 0, 0);
        panel->setSpacing(10);
        m_kmboxNetIp = new QLineEdit;
        panel->addWidget(FormKit::fieldRow(zh(u8"盒子 IP"), m_kmboxNetIp));
        m_kmboxNetPort = new QLineEdit;
        panel->addWidget(FormKit::fieldRow(zh(u8"端口"), m_kmboxNetPort));
        m_kmboxNetUuid = new QLineEdit;
        panel->addWidget(FormKit::fieldRow(zh(u8"UUID / MAC"), m_kmboxNetUuid));
        m_deviceStack->addWidget(page);
    }

    deviceCard->contentLayout()->addWidget(m_deviceStack);
    layout->addWidget(deviceCard);

    loadFieldsFromConfig();

    connect(m_inputMethodCombo, &QComboBox::currentIndexChanged,
            this, &HardwarePage::onInputMethodChanged);
    connect(m_makcuPort, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setMakcuPort(value);
    });
    connect(m_makcuBaud, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int value) {
        ConfigManager::instance().setMakcuBaudrate(value);
    });
    connect(m_makcuNewPort, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setMakcuNewPort(value);
    });
    connect(m_makcuNewBaud, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int value) {
        ConfigManager::instance().setMakcuNewBaudrate(value);
    });
    connect(m_makcuNewPortKbd, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setMakcuNewPortKbd(value);
    });
    connect(m_makcuNewBaudKbd, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int value) {
        ConfigManager::instance().setMakcuNewBaudrateKbd(value);
    });
    // 勾选/取消"接入键盘硬件": 显示或隐藏下面的串口行, 并立即重连生效。
    connect(m_kbdUnitEnabled, &QCheckBox::toggled, this, [this](bool on) {
        m_kbdUnitPanel->setVisible(on);
        ConfigManager::instance().setMakcuNewPortKbd(on ? m_makcuNewPortKbd->text() : QString());
        reconnectDevice();
    });
    connect(m_kmboxNetIp, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setKmboxNetIp(value);
    });
    connect(m_kmboxNetPort, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setKmboxNetPort(value);
    });
    connect(m_kmboxNetUuid, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setKmboxNetUuid(value);
    });
    connect(m_connectBtn, &QPushButton::clicked, this, &HardwarePage::reconnectDevice);

    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, &HardwarePage::loadFieldsFromConfig);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(2000);
    connect(m_statusTimer, &QTimer::timeout, this, &HardwarePage::refreshStatus);
    m_statusTimer->start();

    refreshStatus();
    layout->addStretch();
}

void HardwarePage::loadFieldsFromConfig()
{
    auto& cm = ConfigManager::instance();
    const QString method = cm.inputMethod();
    int index = 0;
    if (method == QStringLiteral("MAKCUNEW")) index = 1;
    else if (method == QStringLiteral("KMBOXNET")) index = 2;

    m_inputMethodCombo->blockSignals(true);
    m_inputMethodCombo->setCurrentIndex(index);
    m_deviceStack->setCurrentIndex(index);
    m_inputMethodCombo->blockSignals(false);

    m_makcuPort->setText(cm.makcuPort());
    m_makcuBaud->setValue(cm.makcuBaudrate());
    m_makcuNewPort->setText(cm.makcuNewPort());
    m_makcuNewBaud->setValue(cm.makcuNewBaudrate());
    m_makcuNewPortKbd->setText(cm.makcuNewPortKbd());
    m_makcuNewBaudKbd->setValue(cm.makcuNewBaudrateKbd());
    // 有端口即视为"接了键盘硬件"。空端口表示没有第二台 -> 不勾选, 面板隐藏。
    {
        const bool on = !cm.makcuNewPortKbd().isEmpty();
        QSignalBlocker block(m_kbdUnitEnabled);   // 加载时不触发重连
        m_kbdUnitEnabled->setChecked(on);
        m_kbdUnitPanel->setVisible(on);
    }
    m_kmboxNetIp->setText(cm.kmboxNetIp());
    m_kmboxNetPort->setText(cm.kmboxNetPort());
    m_kmboxNetUuid->setText(cm.kmboxNetUuid());
}

void HardwarePage::onInputMethodChanged(int index)
{
    m_deviceStack->setCurrentIndex(index);
    if (index >= 0 && index < kInputMethodCount)
        ConfigManager::instance().setInputMethod(QString::fromLatin1(kInputMethodIds[index]));
    reconnectDevice();
}

void HardwarePage::reconnectDevice()
{
    auto& cm = ConfigManager::instance();
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.input_method = cm.inputMethod().toStdString();
        config.makcu_port = cm.makcuPort().toStdString();
        config.makcu_baudrate = cm.makcuBaudrate();
        config.makcu_new_port = cm.makcuNewPort().toStdString();
        config.makcu_new_baudrate = cm.makcuNewBaudrate();
        config.makcu_new_port_kbd = cm.makcuNewPortKbd().toStdString();
        config.makcu_new_baudrate_kbd = cm.makcuNewBaudrateKbd();
        config.kmbox_net_ip = cm.kmboxNetIp().toStdString();
        config.kmbox_net_port = cm.kmboxNetPort().toStdString();
        config.kmbox_net_uuid = cm.kmboxNetUuid().toStdString();
    }

    runtime_config::publish();
    createInputDevices();
    assignInputDevices();
    input_method_changed.store(false);
    refreshStatus();
}

extern MakcuConnection* makcuSerial;
extern MakcuNewConnection* makcuNewSerial;
extern MakcuNewConnection* makcuNewSerialKbd;
extern KmboxNetConnection* kmboxNetSerial;

void HardwarePage::refreshStatus()
{
    const int idx = m_inputMethodCombo ? m_inputMethodCombo->currentIndex() : 0;
    std::lock_guard<std::mutex> deviceLock(inputDeviceMutex);

    bool pointerExists = false;
    bool connected = false;
    QString deviceName;

    switch (idx)
    {
    case 0:
        deviceName = QStringLiteral("MAKCU");
        pointerExists = (makcuSerial != nullptr);
        connected = pointerExists && makcuSerial->isOpen();
        break;
    case 1:
    {
        deviceName = QStringLiteral("MAKCUNEW");
        pointerExists = (makcuNewSerial != nullptr);
        connected = pointerExists && makcuNewSerial->isOpen();
        // 键盘硬件(第二台)是【可选】的: 接了就报"鼠标+键盘", 没接就只报"鼠标"。
        // 不接键盘硬件时键盘注入与自动急停不可用, 但鼠标功能完全不受影响 ——
        // 状态文字要说清这一点, 否则用户会以为设备坏了。
        const bool wantKbd = !ConfigManager::instance().makcuNewPortKbd().isEmpty();
        if (connected && makcuNewSerialKbd != nullptr && makcuNewSerialKbd->isOpen())
            deviceName += zh(u8"(鼠标 + 键盘)");
        else if (connected && wantKbd)
            deviceName += zh(u8"(仅鼠标) — 键盘那台未连上, 键盘注入与自动急停不可用");
        else if (connected && !wantKbd)
            deviceName += zh(u8"(仅鼠标) — 未接键盘硬件");
        break;
    }
    case 2:
        deviceName = QStringLiteral("KMBOXNET");
        pointerExists = (kmboxNetSerial != nullptr);
        connected = pointerExists && kmboxNetSerial->isOpen();
        break;
    default:
        deviceName = zh(u8"未知");
        break;
    }

    if (connected) {
        m_statusDot->setStyleSheet("color:#22C55E; font-size:16px;");
        m_statusText->setText(deviceName + zh(u8" — 已连接"));
        m_statusText->setStyleSheet("color:#22C55E; font-size:13px;");
        m_connectBtn->setText(zh(u8"重连"));
    } else {
        m_statusDot->setStyleSheet("color:#EF4444; font-size:16px;");
        m_statusText->setText(deviceName + (pointerExists
            ? zh(u8" — 连接失败(检查IP/端口/UUID或串口号)")
            : zh(u8" — 未初始化")));
        m_statusText->setStyleSheet("color:#EF4444; font-size:13px;");
        m_connectBtn->setText(zh(u8"连接"));
    }
}
