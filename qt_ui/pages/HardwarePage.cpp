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
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <mutex>

// 串口枚举用的原生 API。放在 Qt 头之后包含, 避免 windows.h 的宏污染 Qt/标准库。
// (工程已在 CMake 里全局定义 WIN32_LEAN_AND_MEAN 与 NOMINMAX)
#ifdef _WIN32
#  include <windows.h>
#endif

#include "mouse/Makcu.h"
#include "mouse/MakcuNew.h"
#include "mouse/kmboxNetConnection.h"
#include "mouse/windows_driver.h"

namespace {

QString zh(const char* text)
{
    return QString::fromUtf8(text);
}

// ── 输入方式 ──────────────────────────────────────────────────────────────
//
// MAKCU      : 纯 ASCII 鼠标固件。可单独用, 也可加一台键盘硬件 -> hybrid。
// MAKCUNEW   : 二进制鼠标固件(与键盘那台同协议), 双硬件由 WrappedMakcuNewDriver 处理。
// KMBOXNET   : 网络盒子。
constexpr const char* kInputMethodIds[] = {"MAKCU", "MAKCUNEW", "KMBOXNET", "FERRUM", "DHZBOX_MINI", "WINDOWS", "CAT"};
constexpr int kInputMethodCount = sizeof(kInputMethodIds) / sizeof(kInputMethodIds[0]);

// ── 串口枚举 ──────────────────────────────────────────────────────────────
//
// ★ 必须用原生 RegEnumValue, 不能用 QSettings。
//
//   SERIALCOMM 这个键的【值名】形如 "\Device\Serial2" —— 自带反斜杠。
//   QSettings 把反斜杠当子键分隔符, 于是 allKeys() 返回的名字会被它当路径去解析,
//   取回来是空值; 结果枚举恒为空, 界面上表现为"串口一直显示(未检测到)"。
//   这里与 makcu SDK 的 SerialPort::getAvailablePorts() 用同一套原生 API,
//   它是被验证过能工作的。
QStringList enumerateComPorts()
{
    QStringList out;
#ifdef Q_OS_WIN
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        char valueName[256];
        char data[256];
        DWORD index = 0;
        for (;;)
        {
            DWORD valueNameSize = sizeof(valueName);
            DWORD dataSize      = sizeof(data);
            DWORD dataType      = 0;

            const LONG r = RegEnumValueA(hKey, index++, valueName, &valueNameSize,
                                         nullptr, &dataType,
                                         reinterpret_cast<BYTE*>(data), &dataSize);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r == ERROR_SUCCESS && dataType == REG_SZ)
            {
                const QString name = QString::fromLatin1(data).trimmed();
                if (!name.isEmpty())
                    out << name;
            }
        }
        RegCloseKey(hKey);
    }
#endif
    out.removeDuplicates();

    // 自然排序: COM2 要排在 COM10 前面(纯字典序会反过来)。
    const auto portNum = [](const QString& s) {
        int i = 0;
        while (i < s.size() && !s.at(i).isDigit()) ++i;
        return s.mid(i).toInt();
    };
    std::sort(out.begin(), out.end(), [&](const QString& a, const QString& b) {
        const int na = portNum(a), nb = portNum(b);
        return na != nb ? na < nb : a < b;
    });
    return out;
}

// 波特率候选档位。
//   MAKCU(SDK): 建链固定 115200, 成功后 SDK 自己切高速; 这里选的是随后 setBaudRate 的目标。
//   MAKCUNEW  : 固件上电固定 115200, 由固件协商到目标速率。
constexpr int kBaudPresets[] = {
    115200, 921600, 1000000, 1500000, 2000000, 3000000, 4000000, 6000000
};

// 串口下拉。真值放在 itemData 里 —— "未检测到"的保留项显示文本带后缀,
// 但取值必须干净, 否则会把后缀写进配置。
QComboBox* makePortCombo()
{
    auto* box = new QComboBox;
    box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    return box;
}

QComboBox* makeBaudCombo()
{
    auto* box = new QComboBox;
    for (int b : kBaudPresets)
        box->addItem(QString::number(b), b);
    return box;
}

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
        ,QStringLiteral("Ferrum")
        ,zh(u8"DHZBox Mini（网络）")
        ,zh(u8"Windows 原生输入（SendInput）")
        ,zh(u8"CAT（加密网络）")
    });
    inputCard->contentLayout()->addWidget(
        FormKit::fieldRow(zh(u8"方式"), m_inputMethodCombo));

    // 串口/波特率现在都是下拉。如果设备是插好之后才打开本界面的, 点这里重新枚举。
    {
        auto* refreshRow = new QHBoxLayout;
        m_refreshPortsBtn = new QPushButton(zh(u8"刷新串口列表"));
        m_refreshPortsBtn->setFixedHeight(28);
        m_refreshPortsBtn->setCursor(Qt::PointingHandCursor);
        refreshRow->addWidget(m_refreshPortsBtn);
        refreshRow->addStretch();
        inputCard->contentLayout()->addLayout(refreshRow);
    }
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
    m_connectBtn = new QPushButton(zh(u8"连接鼠标"));
    m_connectBtn->setFixedHeight(28);
    m_connectBtn->setCursor(Qt::PointingHandCursor);
    m_connectBtn->setToolTip(zh(u8"只重连鼠标那台。键盘那台用下面键盘卡片里的按钮单独连。"));
    statusRow->addWidget(m_connectBtn);
    statusCard->contentLayout()->addLayout(statusRow);
    layout->addWidget(statusCard);

    auto* deviceCard = new CardWidget(zh(u8"设备参数"), QStringLiteral("adjustments"));
    m_deviceStack = new QStackedWidget;

    // ── 键盘硬件(第二台): 【共用一套控件】 ────────────────────────────────
    //
    // MAKCU(hybrid) 与 MAKCUNEW 两种方式都需要"接一台键盘硬件"这个开关, 且
    // 读写的是同一份配置 (makcu_new_port_kbd)。如果每个面板各建一套控件, 除了
    // 重复占内存, 更麻烦的是成员指针会被后建的覆盖 —— loadFieldsFromConfig 与
    // refreshStatus 只会操作最后一个, 前一个面板上的勾选就变成"点了没反应"。
    //
    // 因此这里只创建一次, 两个面板各自 addWidget 同一个 QWidget(同一父控件下
    // 一个 widget 只能在布局里出现一次, 所以用两个容器各放一份"行"不现实)——
    // 改用 QStackedWidget 天然只显示当前页, 同一实例加到两页会出问题。
    // 结论: 共用一个实例, 归属到 deviceStack 之外, 由 onInputMethodChanged
    // 控制其可见性, 面板内只放占位说明。
    auto ensureKbdWidgets = [this]() {
        if (m_kbdUnitEnabled) return;

        m_kbdUnitEnabled = new QCheckBox(zh(u8"接入键盘硬件(第二台)"));
        m_kbdUnitEnabled->setToolTip(zh(
            u8"勾选后键盘注入与自动急停走这一台;\n"
            u8"不勾选则键盘注入与自动急停不可用, 鼠标功能不受影响。"));

        m_kbdUnitPanel = new QWidget;
        auto* kbdPanel = new QVBoxLayout(m_kbdUnitPanel);
        kbdPanel->setContentsMargins(0, 0, 0, 0);
        kbdPanel->setSpacing(10);

        m_makcuNewPortKbd = makePortCombo();
        kbdPanel->addWidget(FormKit::fieldRow(zh(u8"串口"), m_makcuNewPortKbd));
        m_makcuNewBaudKbd = makeBaudCombo();
        kbdPanel->addWidget(FormKit::fieldRow(zh(u8"波特率"), m_makcuNewBaudKbd));
    };

    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0, 0, 0, 0);
        panel->setSpacing(10);

        // ---- 鼠标硬件 (官方 ASCII 固件) ----
        auto* mouseTitle = new QLabel(zh(u8"鼠标"));
        mouseTitle->setStyleSheet("font-weight:600; font-size:13px;");
        panel->addWidget(mouseTitle);

        m_makcuPort = makePortCombo();
        panel->addWidget(FormKit::fieldRow(zh(u8"串口"), m_makcuPort));
        m_makcuBaud = makeBaudCombo();
        panel->addWidget(FormKit::fieldRow(zh(u8"波特率"), m_makcuBaud));

        ensureKbdWidgets();
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

        m_makcuNewPort = makePortCombo();
        panel->addWidget(FormKit::fieldRow(zh(u8"串口"), m_makcuNewPort));
        m_makcuNewBaud = makeBaudCombo();
        panel->addWidget(FormKit::fieldRow(zh(u8"波特率"), m_makcuNewBaud));

        ensureKbdWidgets();
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
        m_kmboxNetUuid->setPlaceholderText(zh(u8"盒子屏幕上的 8 位十六进制 UUID"));
        panel->addWidget(FormKit::fieldRow(zh(u8"UUID（8 位十六进制）"), m_kmboxNetUuid));
        m_deviceStack->addWidget(page);
    }

    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0, 0, 0, 0);
        panel->setSpacing(10);
        m_ferrumPort = makePortCombo();
        m_ferrumBaud = makeBaudCombo();
        panel->addWidget(FormKit::fieldRow(zh(u8"Ferrum 串口"), m_ferrumPort));
        panel->addWidget(FormKit::fieldRow(zh(u8"目标波特率"), m_ferrumBaud));
        const QString ferrumHint = zh(u8"优先选择 Ferrum App 提供的虚拟串口（新版 Software API，支持键鼠）。直连 CP210x 为旧 Legacy API，仅支持鼠标，设备重上电后波特率为 115200。");
        page->setToolTip(ferrumHint);
        m_ferrumPort->setToolTip(ferrumHint);
        m_ferrumBaud->setToolTip(ferrumHint);
        m_deviceStack->addWidget(page);
    }

    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0, 0, 0, 0);
        panel->setSpacing(10);
        m_dhzboxIp = new QLineEdit;
        m_dhzboxPort = new QSpinBox;
        m_dhzboxPort->setRange(1, 65535);
        m_dhzboxKey = new QSpinBox;
        m_dhzboxKey->setRange(0, 255);
        panel->addWidget(FormKit::fieldRow(zh(u8"盒子 IP"), m_dhzboxIp));
        panel->addWidget(FormKit::fieldRow(zh(u8"端口"), m_dhzboxPort));
        panel->addWidget(FormKit::fieldRow(zh(u8"Rand 密钥"), m_dhzboxKey));
        const QString hint = zh(u8"适用 DHZBox Mini 的 UDP 协议。发送端就绪不代表已收到设备确认；请以实际鼠标动作验证。");
        page->setToolTip(hint);
        m_dhzboxIp->setToolTip(hint);
        m_deviceStack->addWidget(page);
    }

    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0,0,0,0);
        page->setToolTip(zh(u8"无需外接硬件，瞄准与宏通过 Windows 系统接口发送键盘、鼠标和滚轮。输入作用于运行本程序的这台电脑。\n"
            u8"无需填写串口或网络参数。原生输入不支持屏蔽真实键盘；目标程序是否接受系统模拟输入取决于该程序。"));
        m_deviceStack->addWidget(page);
    }
    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0,0,0,0);
        panel->setSpacing(10);
        m_catIp = new QLineEdit;
        m_catPort = new QSpinBox; m_catPort->setRange(1,65535);
        m_catUuid = new QLineEdit; m_catUuid->setMaxLength(8);
        m_catUuid->setPlaceholderText(zh(u8"盒子上的 8 位十六进制 UUID"));
        m_catMonitorPort = new QSpinBox; m_catMonitorPort->setRange(1,65535);
        panel->addWidget(FormKit::fieldRow(zh(u8"CAT IP"),m_catIp));
        panel->addWidget(FormKit::fieldRow(zh(u8"命令端口"),m_catPort));
        m_catUuid->setPlaceholderText(zh(u8"1–8 位十六进制 UUID，可带 0x"));
        panel->addWidget(FormKit::fieldRow(zh(u8"UUID"),m_catUuid));
        panel->addWidget(FormKit::fieldRow(zh(u8"本机监听端口"),m_catMonitorPort));
        const QString catHint = zh(u8"UUID 按设备显示的十六进制数字填写；命令端口是盒子端口，监听端口是本机接收端口。两端口须不同且本机可用。支持键鼠按键、物理监听与按键屏蔽；当前协议未定义轴屏蔽和滚轮输出。");
        page->setToolTip(catHint);
        m_catUuid->setToolTip(catHint);
        m_deviceStack->addWidget(page);
    }
    deviceCard->contentLayout()->addWidget(m_deviceStack);
    layout->addWidget(deviceCard);

    // ── 键盘硬件卡片(共用, 在 deviceStack 之外) ──────────────────────────
    //
    // 放在 QStackedWidget 之外, 是因为 MAKCU(hybrid) 与 MAKCUNEW 两种方式共用
    // 同一份配置与同一套控件; 放进各自的页里就必须复制控件, 成员指针互相覆盖。
    // 它只对这两种方式有意义, KMBOXNET 下由 onInputMethodChanged 隐藏。
    m_kbdCard = new CardWidget(zh(u8"键盘硬件(第二台)"), QStringLiteral("keyboard"));
    {
        auto* kbdLayout = m_kbdCard->contentLayout();

        const QString kbdHint = zh(
            u8"独立的一台硬件, 真实键盘插在它上面。键盘注入与自动急停的屏蔽命令都只走这台,"
            u8"因此不接它也不影响鼠标位移/按键/滚轮 —— 只是键盘功能不可用。");
        m_kbdCard->setToolTip(kbdHint);
        m_kbdUnitEnabled->setToolTip(kbdHint);

        kbdLayout->addWidget(m_kbdUnitEnabled);
        kbdLayout->addWidget(m_kbdUnitPanel);

        // ── 键盘独立连接状态 + 独立连接按钮 ─────────────────────────────
        //
        // 与鼠标分开: 两块板是各自独立的串口/固件, 一个连不上不该影响另一个的
        // 重连操作, 状态也必须能分别显示。原来只有一个总的"连接"按钮, 一旦
        // 键盘那台有问题, 用户无法判断是鼠标还是键盘挂了。
        auto* kbdStatusRow = new QHBoxLayout;
        kbdStatusRow->setSpacing(8);
        m_kbdStatusDot = new QLabel(QString::fromUtf8(u8"●"));
        m_kbdStatusDot->setFixedWidth(20);
        kbdStatusRow->addWidget(m_kbdStatusDot);
        m_kbdStatusText = new QLabel;
        m_kbdStatusText->setStyleSheet("font-size:13px;");
        kbdStatusRow->addWidget(m_kbdStatusText, 1);
        m_connectKbdBtn = new QPushButton(zh(u8"连接键盘"));
        m_connectKbdBtn->setFixedHeight(28);
        m_connectKbdBtn->setCursor(Qt::PointingHandCursor);
        kbdStatusRow->addWidget(m_connectKbdBtn);
        kbdLayout->addLayout(kbdStatusRow);
    }
    layout->addWidget(m_kbdCard);

    // 顺序很重要: 先把串口列表灌进下拉, 再 loadFieldsFromConfig 去选中配置值。
    refreshPortLists();
    loadFieldsFromConfig();

    connect(m_inputMethodCombo, &QComboBox::currentIndexChanged,
            this, &HardwarePage::onInputMethodChanged);
    connect(m_refreshPortsBtn, &QPushButton::clicked, this, [this] {
        refreshPortLists();   // 只重灌下拉, 不自动重连(避免每次刷新都重开串口)
    });
    connect(m_makcuPort, &QComboBox::currentIndexChanged, this, [this](int) {
        ConfigManager::instance().setMakcuPort(comboText(m_makcuPort));
    });
    connect(m_makcuBaud, &QComboBox::currentIndexChanged, this, [this](int) {
        ConfigManager::instance().setMakcuBaudrate(comboNumber(m_makcuBaud));
    });
    connect(m_makcuNewPort, &QComboBox::currentIndexChanged, this, [this](int) {
        ConfigManager::instance().setMakcuNewPort(comboText(m_makcuNewPort));
    });
    connect(m_makcuNewBaud, &QComboBox::currentIndexChanged, this, [this](int) {
        ConfigManager::instance().setMakcuNewBaudrate(comboNumber(m_makcuNewBaud));
    });
    connect(m_makcuNewPortKbd, &QComboBox::currentIndexChanged, this, [this](int) {
        ConfigManager::instance().setMakcuNewPortKbd(comboText(m_makcuNewPortKbd));
    });
    connect(m_makcuNewBaudKbd, &QComboBox::currentIndexChanged, this, [this](int) {
        ConfigManager::instance().setMakcuNewBaudrateKbd(comboNumber(m_makcuNewBaudKbd));
    });
    // 勾选/取消"接入键盘硬件": 显示或隐藏下面的串口行, 并立即重连键盘那一台。
    connect(m_kbdUnitEnabled, &QCheckBox::toggled, this, [this](bool on) {
        m_kbdUnitPanel->setVisible(on);
        ConfigManager::instance().setMakcuNewPortKbd(on ? comboText(m_makcuNewPortKbd) : QString());
        reconnectKbdOnly();
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
    connect(m_ferrumPort, &QComboBox::currentIndexChanged, this, [this](int) {
        ConfigManager::instance().setFerrumPort(comboText(m_ferrumPort));
    });
    connect(m_ferrumBaud, &QComboBox::currentIndexChanged, this, [this](int) {
        ConfigManager::instance().setFerrumBaudrate(comboNumber(m_ferrumBaud));
    });
    connect(m_dhzboxIp, &QLineEdit::textChanged, this, [](const QString& v) { ConfigManager::instance().setDhzboxIp(v); });
    connect(m_dhzboxPort, &QSpinBox::valueChanged, this, [](int v) { ConfigManager::instance().setDhzboxPort(v); });
    connect(m_dhzboxKey, &QSpinBox::valueChanged, this, [](int v) { ConfigManager::instance().setDhzboxKey(v); });
    connect(m_catIp, &QLineEdit::textChanged, this, [](const QString& v) { ConfigManager::instance().setCatIp(v); });
    connect(m_catPort, &QSpinBox::valueChanged, this, [](int v) { ConfigManager::instance().setCatPort(v); });
    connect(m_catUuid, &QLineEdit::textChanged, this, [](const QString& v) { ConfigManager::instance().setCatUuid(v); });
    connect(m_catMonitorPort, &QSpinBox::valueChanged, this, [](int v) { ConfigManager::instance().setCatMonitorPort(v); });
    // 两个独立按钮: 鼠标一台、键盘一台, 互不影响。
    connect(m_connectBtn, &QPushButton::clicked, this, &HardwarePage::reconnectMouseOnly);
    connect(m_connectKbdBtn, &QPushButton::clicked, this, &HardwarePage::reconnectKbdOnly);

    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, &HardwarePage::loadFieldsFromConfig);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(2000);
    connect(m_statusTimer, &QTimer::timeout, this, &HardwarePage::refreshStatus);
    m_statusTimer->start();

    refreshStatus();
    layout->addStretch();
}

void HardwarePage::updateKbdCardVisibility(int methodIndex)
{
    // 键盘硬件只在 MAKCU(hybrid) 与 MAKCUNEW 下有意义:
    //   · MAKCU    -> WrappedHybridDriver(鼠标 ASCII + 键盘二进制)
    //   · MAKCUNEW -> WrappedMakcuNewDriver(两台同为二进制)
    // KMBOXNET 走网络盒子, 没有"第二台串口"的概念。
    const bool supportsKbd = (methodIndex == 0 || methodIndex == 1);
    if (m_kbdCard)
        m_kbdCard->setVisible(supportsKbd);
}

void HardwarePage::loadFieldsFromConfig()
{
    auto& cm = ConfigManager::instance();
    const QString method = cm.inputMethod();
    int index = 0;
    if (method == QStringLiteral("MAKCUNEW")) index = 1;
    else if (method == QStringLiteral("KMBOXNET")) index = 2;
    else if (method == QStringLiteral("FERRUM")) index = 3;
    else if (method == QStringLiteral("DHZBOX_MINI")) index = 4;
    else if (method == QStringLiteral("WINDOWS")) index = 5;
    else if (method == QStringLiteral("CAT")) index = 6;

    m_inputMethodCombo->blockSignals(true);
    m_inputMethodCombo->setCurrentIndex(index);
    m_deviceStack->setCurrentIndex(index);
    m_inputMethodCombo->blockSignals(false);
    m_inputMethodCombo->setToolTip(m_deviceStack->currentWidget()->toolTip());
    updateKbdCardVisibility(index);

    // 下拉按【值】选中, 不按文本 —— "未检测到"的保留项显示文本带后缀, 与配置值不同。
    const auto selectByValue = [](QComboBox* box, const QVariant& value) {
        if (!box) return;
        QSignalBlocker block(box);          // 加载时不触发回写
        const int idx = box->findData(value);
        box->setCurrentIndex(idx >= 0 ? idx : 0);
    };

    selectByValue(m_makcuPort, cm.makcuPort());
    selectByValue(m_makcuBaud, cm.makcuBaudrate());
    selectByValue(m_makcuNewPort, cm.makcuNewPort());
    selectByValue(m_makcuNewBaud, cm.makcuNewBaudrate());
    selectByValue(m_makcuNewPortKbd, cm.makcuNewPortKbd());
    selectByValue(m_makcuNewBaudKbd, cm.makcuNewBaudrateKbd());
    selectByValue(m_ferrumPort, cm.ferrumPort());
    selectByValue(m_ferrumBaud, cm.ferrumBaudrate());
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
    m_dhzboxIp->setText(cm.dhzboxIp());
    m_dhzboxPort->setValue(cm.dhzboxPort());
    m_catIp->setText(cm.catIp());
    m_catPort->setValue(cm.catPort());
    m_catUuid->setText(cm.catUuid());
    m_catMonitorPort->setValue(cm.catMonitorPort());
    m_dhzboxKey->setValue(cm.dhzboxKey());
}

QString HardwarePage::comboText(const QComboBox* box)
{
    if (!box) return QString();
    const QVariant v = box->currentData();
    return v.isValid() ? v.toString() : box->currentText();
}

int HardwarePage::comboNumber(const QComboBox* box)
{
    if (!box) return 0;
    bool ok = false;
    const int n = (box->currentData().isValid() ? box->currentData() : box->currentText())
                      .toString().toInt(&ok);
    return ok ? n : 0;
}

void HardwarePage::refreshPortLists()
{
    const QStringList ports = enumerateComPorts();

    // 灌一个串口下拉: 首项空(=不配置), 然后列出检测到的口, 最后把"配置里有但当前
    // 没检测到"的值也补上 —— 否则设备没插时打开界面, 会把用户配好的串口冲成空。
    const auto fill = [&](QComboBox* box, const QString& configured) {
        if (!box) return;
        const QString keep = configured.isEmpty() ? comboText(box) : configured;

        QSignalBlocker block(box);
        box->clear();
        box->addItem(zh(u8"(不配置)"), QString());
        for (const QString& p : ports)
            box->addItem(p, p);

        if (!keep.isEmpty() && box->findData(keep) < 0)
            box->addItem(keep + zh(u8"  (未检测到)"), keep);

        const int idx = box->findData(keep);
        box->setCurrentIndex(idx >= 0 ? idx : 0);
    };

    auto& cm = ConfigManager::instance();
    fill(m_makcuPort,       cm.makcuPort());
    fill(m_makcuNewPort,    cm.makcuNewPort());
    fill(m_makcuNewPortKbd, cm.makcuNewPortKbd());
    fill(m_ferrumPort, cm.ferrumPort());

    // 波特率: 档位是固定的, 但配置值可能不在档位里(手改过 ini) —— 补进去, 避免被冲掉。
    const auto ensureBaud = [&](QComboBox* box, int value) {
        if (!box || value <= 0) return;
        QSignalBlocker block(box);
        if (box->findData(value) < 0)
            box->addItem(QString::number(value), value);
    };
    ensureBaud(m_makcuBaud,        cm.makcuBaudrate());
    ensureBaud(m_makcuNewBaud,     cm.makcuNewBaudrate());
    ensureBaud(m_makcuNewBaudKbd,  cm.makcuNewBaudrateKbd());
    ensureBaud(m_ferrumBaud, cm.ferrumBaudrate());
}

void HardwarePage::onInputMethodChanged(int index)
{
    m_deviceStack->setCurrentIndex(index);
    m_inputMethodCombo->setToolTip(m_deviceStack->currentWidget()->toolTip());
    updateKbdCardVisibility(index);
    if (index >= 0 && index < kInputMethodCount)
        ConfigManager::instance().setInputMethod(QString::fromLatin1(kInputMethodIds[index]));
    reconnectDevice();
}

void HardwarePage::syncConfigToRuntime()
{
    auto& cm = ConfigManager::instance();
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
    config.ferrum_port = cm.ferrumPort().toStdString();
    config.ferrum_baudrate = cm.ferrumBaudrate();
    config.dhzbox_ip = cm.dhzboxIp().toStdString();
    config.dhzbox_port = cm.dhzboxPort();
    config.cat_ip = cm.catIp().toStdString();
    config.cat_port = cm.catPort();
    config.cat_uuid = cm.catUuid().toStdString();
    config.cat_monitor_port = cm.catMonitorPort();
    config.dhzbox_key = cm.dhzboxKey();
}

void HardwarePage::reconnectDevice()
{
    syncConfigToRuntime();
    runtime_config::publish();
    createInputDevices();
    assignInputDevices();
    input_method_changed.store(false);
    refreshStatus();
}

void HardwarePage::reconnectMouseOnly()
{
    // 只重连鼠标那台: 键盘的串口/连接状态完全不动。
    // 这是"分开连"的关键 —— 键盘那台有问题时, 重连鼠标不该把键盘也一起拆掉重来。
    syncConfigToRuntime();
    runtime_config::publish();
    reconnectMouseDevice();
    input_method_changed.store(false);
    refreshStatus();
}

void HardwarePage::reconnectKbdOnly()
{
    // 只重连键盘那台。
    syncConfigToRuntime();
    runtime_config::publish();
    reconnectKeyboardDevice();
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
    {
        // MAKCU = 官方 ASCII 鼠标固件。填了键盘口则进入【混合模式】:
        // 鼠标走 MakcuConnection(ASCII), 键盘走 MakcuNewConnection(二进制)。
        deviceName = QStringLiteral("MAKCU");
        pointerExists = (makcuSerial != nullptr);
        connected = pointerExists && makcuSerial->isOpen();

        const bool wantKbd = !ConfigManager::instance().makcuNewPortKbd().isEmpty();
        if (connected && makcuNewSerialKbd != nullptr && makcuNewSerialKbd->isOpen())
            deviceName += zh(u8"(混合模式: 鼠标 ASCII + 键盘二进制)");
        else if (connected && wantKbd)
            deviceName += zh(u8"(仅鼠标) — 键盘那台未连上, 键盘注入与自动急停不可用");
        else if (connected && !wantKbd)
            deviceName += zh(u8"(仅鼠标) — 未接键盘硬件");
        break;
    }
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
    case 3:
        deviceName = QStringLiteral("Ferrum");
        pointerExists = ferrumDriver != nullptr;
        connected = pointerExists && ferrumDriver->isOpen();
        break;
    case 4:
        deviceName = zh(u8"DHZBox Mini");
        pointerExists = dhzboxDriver != nullptr;
        connected = pointerExists && dhzboxDriver->isOpen();
        break;
    case 6:
        deviceName = QStringLiteral("CAT");
        pointerExists = catDriver != nullptr;
        connected = pointerExists && catDriver->isOpen();
        break;
    case 5:
        deviceName = zh(u8"Windows 原生输入");
        pointerExists = windowsDriver != nullptr;
        connected = pointerExists && windowsDriver->isOpen();
        break;
    default:
        deviceName = zh(u8"未知");
        break;
    }

    if (connected) {
        const QString color = idx == 4 ? QStringLiteral("#F59E0B") : QStringLiteral("#22C55E");
        m_statusDot->setStyleSheet("color:" + color + "; font-size:16px;");
        QString status = idx == 4 ? zh(u8" — UDP 发送端就绪（设备未确认）") : zh(u8" — 已连接");
        if (idx == 4 && !dhzboxLastError.empty())
            status += zh(u8"；物理按键监听不可用");
        if (idx == 3) status += (ferrumDriver->capabilities() & mouse_driver::kCapKeyboard)
            ? zh(u8"（Software API，键鼠）") : zh(u8"（Legacy API，仅鼠标）");
        if (idx == 5) {
            status = zh(u8" — 系统接口就绪");
            if (!windowsDriver->lastError().empty()) status += zh(u8"；") + QString::fromStdString(windowsDriver->lastError());
        }
        m_statusText->setText(deviceName + status);
        m_statusText->setStyleSheet("color:" + color + "; font-size:13px;");
        m_connectBtn->setText(zh(u8"重连鼠标"));
    } else {
        m_statusDot->setStyleSheet("color:#EF4444; font-size:16px;");
        if (idx == 2 && !kmboxNetLastError.empty())
            m_statusText->setText(deviceName + zh(u8" — 连接失败: ")
                                  + QString::fromStdString(kmboxNetLastError));
        else if (idx == 3 && !ferrumLastError.empty())
            m_statusText->setText(deviceName + zh(u8" — 连接失败: ")
                                  + QString::fromStdString(ferrumLastError));
        else if (idx == 6 && !catLastError.empty())
            m_statusText->setText(deviceName + zh(u8" — 连接失败: ") + QString::fromStdString(catLastError));
        else if (idx == 4 && !dhzboxLastError.empty())
            m_statusText->setText(deviceName + zh(u8" — 初始化失败: ")
                                  + QString::fromStdString(dhzboxLastError));
        else
            m_statusText->setText(deviceName + (pointerExists
                ? zh(u8" — 连接失败(检查IP/端口/UUID或串口号)")
                : zh(u8" — 未初始化")));
        m_statusText->setStyleSheet("color:#EF4444; font-size:13px;");
        m_connectBtn->setText(zh(u8"连接鼠标"));
    }
    if (idx == 5) {
        m_connectBtn->setText(zh(u8"重新初始化"));
        if (pointerExists && !connected) m_statusText->setText(deviceName + zh(u8" — ") + QString::fromStdString(windowsDriver->lastError()));
    }

    // ── 键盘那台的状态, 独立显示在键盘卡片里 ─────────────────────────────
    //
    // 与鼠标完全分开: 两块板是独立串口/固件, 必须能单独看出是哪一台没连上。
    // 之前只有一个总状态, 键盘挂了也只会显示在鼠标那一行上, 无法区分。
    if (m_kbdStatusText)
    {
        const QString kbdPort = ConfigManager::instance().makcuNewPortKbd();
        const bool kbdConfigured = !kbdPort.isEmpty();
        const bool kbdOpen = (makcuNewSerialKbd != nullptr) && makcuNewSerialKbd->isOpen();

        if (kbdOpen)
        {
            m_kbdStatusDot->setStyleSheet("color:#22C55E; font-size:16px;");
            m_kbdStatusText->setText(kbdPort + zh(u8" — 已连接"));
            m_kbdStatusText->setStyleSheet("color:#22C55E; font-size:13px;");
            m_connectKbdBtn->setText(zh(u8"重连键盘"));
            m_connectKbdBtn->setEnabled(true);
        }
        else if (kbdConfigured)
        {
            m_kbdStatusDot->setStyleSheet("color:#EF4444; font-size:16px;");
            m_kbdStatusText->setText(kbdPort + zh(u8" — 连接失败(检查串口号/供电)"));
            m_kbdStatusText->setStyleSheet("color:#EF4444; font-size:13px;");
            m_connectKbdBtn->setText(zh(u8"连接键盘"));
            m_connectKbdBtn->setEnabled(true);
        }
        else
        {
            m_kbdStatusDot->setStyleSheet("color:#9CA3AF; font-size:16px;");
            m_kbdStatusText->setText(zh(u8"未配置串口 — 勾选上面的开关并选串口"));
            m_kbdStatusText->setStyleSheet("color:#ABA697; font-size:13px;");
            m_connectKbdBtn->setText(zh(u8"连接键盘"));
            m_connectKbdBtn->setEnabled(false);
        }
    }
}
