#include "pages/LanTuningPage.h"
#include "remote/LanTuningServer.h"
#include "widgets/CardWidget.h"
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

LanTuningPage::LanTuningPage(QWidget* parent) : QWidget(parent) {
    auto* server = new LanTuningServer(this);
    connect(qApp, &QCoreApplication::aboutToQuit, server, &LanTuningServer::stop);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* body = new QWidget;
    scroll->setWidget(body);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(16);
    auto* card = new CardWidget(QStringLiteral("局域网调参"), QStringLiteral("device-desktop"), this);
    auto* content = card->contentLayout();
    const QString intro = QStringLiteral("在同一局域网的另一台电脑上，通过浏览器调整当前配置。网页内点击“应用并保存”后生效，本机界面同步更新。");
    card->setToolTip(intro);
    auto* row = new QHBoxLayout;
    auto* enabled = new QCheckBox(QStringLiteral("开启局域网调参"));
    enabled->setToolTip(intro);
    enabled->setObjectName("lanTuningEnabled");
    auto* port = new QSpinBox;
    port->setObjectName("lanTuningPort");
    port->setRange(1024, 65535);
    QSettings settings("Apotheosis", "LanTuning");
    port->setValue(settings.value("port", 17890).toInt());
    row->addWidget(enabled);
    row->addStretch();
    row->addWidget(new QLabel(QStringLiteral("端口")));
    row->addWidget(port);
    content->addLayout(row);
    auto* addresses = new QComboBox;
    addresses->setObjectName("lanTuningAddresses");
    addresses->setToolTip(QStringLiteral("选择与另一台电脑处于同一网段的地址。"));
    content->addWidget(addresses);
    auto* link = new QLineEdit;
    link->setReadOnly(true);
    link->setObjectName("lanTuningUrl");
    link->setPlaceholderText(QStringLiteral("开启后生成访问链接"));
    content->addWidget(link);
    auto* actions = new QHBoxLayout;
    auto* copy = new QPushButton(QStringLiteral("复制完整地址"));
    auto* open = new QPushButton(QStringLiteral("本机打开网页"));
    auto* refresh = new QPushButton(QStringLiteral("刷新网卡地址"));
    actions->addWidget(copy); actions->addWidget(open); actions->addWidget(refresh); actions->addStretch();
    content->addLayout(actions);
    auto* status = new QLabel(QStringLiteral("已关闭"));
    status->setWordWrap(true);
    content->addWidget(status);
    layout->addWidget(card);
    const QString helpText = QStringLiteral(
        "1. 开启服务，复制上面的完整地址。\n"
        "2. 在另一台电脑的浏览器地址栏粘贴打开，不需要安装软件或复制 HTML 文件。\n"
        "3. 选择热键配置，修改参数后点击“应用并保存”。\n\n"
        "支持三套 PID、跟随补偿、FOV、轴屏蔽、动态 FOV、默认与类别瞄点、识别阈值、小目标增强、准星找色与已有 HSV 颜色范围。\n\n"
        "程序需保持运行。若 Windows 防火墙提示，请允许专用网络访问；无需关闭防火墙。127.0.0.1 仅供本机使用。\n"
        "链接含本次访问凭据，关闭后失效；重新开启需重新复制。服务每次启动程序时默认关闭。");
    copy->setToolTip(helpText);
    link->setToolTip(helpText);
    card->setToolTip(intro + "\n\n" + helpText);
    layout->addStretch();
    auto refreshAddresses = [server, addresses, link] {
        const QString selected = addresses->currentText();
        addresses->clear();
        for (const auto& url : server->urls()) {
            QUrl parsed(url);
            const QString label = parsed.host() + (parsed.host() == "127.0.0.1" ? QStringLiteral("（仅本机）") : QString());
            addresses->addItem(label, url);
        }
        const int index = addresses->findText(selected);
        if (index >= 0) addresses->setCurrentIndex(index);
        link->setText(addresses->currentData().toString());
    };
    auto updateButtons = [=] {
        const bool on = server->running();
        port->setEnabled(!on); addresses->setEnabled(on);
        copy->setEnabled(on); open->setEnabled(on); refresh->setEnabled(on);
    };
    connect(addresses, &QComboBox::currentIndexChanged, this, [=](int) {
        link->setText(addresses->currentData().toString());
    });
    connect(enabled, &QCheckBox::toggled, this, [=](bool on) {
        if (on) {
            QString error;
            if (!server->start(static_cast<quint16>(port->value()), error)) {
                QSignalBlocker blocker(enabled);
                enabled->setChecked(false);
                status->setText(QStringLiteral("开启失败：") + error);
            } else {
                QSettings s("Apotheosis", "LanTuning");
                s.setValue("port", port->value());
            }
        } else { server->stop(); status->setText(QStringLiteral("已关闭，访问凭据已失效")); }
        refreshAddresses(); updateButtons();
    });
    connect(copy, &QPushButton::clicked, this, [=] { qApp->clipboard()->setText(link->text()); });
    connect(open, &QPushButton::clicked, this, [=] { QDesktopServices::openUrl(QUrl(link->text())); });
    connect(refresh, &QPushButton::clicked, this, refreshAddresses);
    connect(server, &LanTuningServer::activity, status, &QLabel::setText);
    updateButtons();
}
