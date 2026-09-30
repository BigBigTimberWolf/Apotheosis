#include "pages/HeadBodyFusionPage.h"
#include "Apotheosis.h"
#include "config/config_bridge.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace {
QLabel* hint(const QString& value) {
    auto* label = new QLabel(value);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color:#71717A;font-size:12px;"));
    return label;
}
}

HeadBodyFusionPage::HeadBodyFusionPage(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    auto* card = new CardWidget(QStringLiteral("头身融合"), QStringLiteral("target"));
    enabled_ = new QCheckBox(QStringLiteral("启用头身融合"));
    card->contentLayout()->addWidget(enabled_);
    const QString fusionHelp = QStringLiteral(
        "同一目标同时识别出头框和身体框时，只把身体框交给选靶和 PID。"
        "因此继续使用身体类别的瞄点、优先级和识别框；没有配对的框保持原样。"
        "请在目标类别中把身体类别设为“瞄准”，并把它加入热键的类别列表。"
        "头和身体必须是不同的模型类别。");
    card->setToolTip(fusionHelp);
    enabled_->setToolTip(fusionHelp);
    head_ = new QComboBox;
    body_ = new QComboBox;
    card->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("头类别"), head_));
    card->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("身体类别"), body_));
    status_ = hint(QString());
    card->contentLayout()->addWidget(status_);
    head_->setToolTip(fusionHelp);
    body_->setToolTip(fusionHelp);
    layout->addWidget(card);
    layout->addStretch();

    connect(enabled_, &QCheckBox::toggled, this, [this] { commit(); });
    connect(head_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { commit(); });
    connect(body_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { commit(); });
    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, [this] { classFingerprint_ = std::numeric_limits<std::size_t>::max(); refresh(); });
    poll_ = new QTimer(this);
    poll_->setInterval(500);
    connect(poll_, &QTimer::timeout, this, [this] { refresh(); });
    poll_->start();
    refresh();
}

void HeadBodyFusionPage::refresh() {
    struct Entry { int id; QString name; };
    std::vector<Entry> entries;
    bool enabled;
    int headId, bodyId;
    std::size_t fingerprint = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        enabled = config.head_body_fusion_enabled;
        headId = config.head_body_head_class_id;
        bodyId = config.head_body_body_class_id;
        for (const auto& item : config.class_filters) {
            const auto name = QString::fromStdString(item.class_name);
            entries.push_back({item.class_id, name});
            fingerprint ^= std::hash<int>{}(item.class_id) + 0x9e3779b9
                + (fingerprint << 6) + (fingerprint >> 2);
            fingerprint ^= std::hash<std::string>{}(item.class_name) + 0x9e3779b9
                + (fingerprint << 6) + (fingerprint >> 2);
        }
    }
    const auto keepSavedId = [&](int id) {
        if (id < 0 || std::any_of(entries.begin(), entries.end(),
            [id](const Entry& entry) { return entry.id == id; })) return;
        entries.push_back({id, QStringLiteral("已保存类别（当前模型未加载）")});
        fingerprint ^= std::hash<int>{}(id) + 0x9e3779b9
            + (fingerprint << 6) + (fingerprint >> 2);
    };
    keepSavedId(headId);
    keepSavedId(bodyId);
    QSignalBlocker enabledBlock(enabled_);
    enabled_->setChecked(enabled);
    if (fingerprint != classFingerprint_ || head_->count() == 0) {
        classFingerprint_ = fingerprint;
        QSignalBlocker headBlock(head_);
        QSignalBlocker bodyBlock(body_);
        head_->clear(); body_->clear();
        head_->addItem(QStringLiteral("请选择头类别"), -1);
        body_->addItem(QStringLiteral("请选择身体类别"), -1);
        for (const auto& entry : entries) {
            const auto label = QStringLiteral("%1 · %2").arg(entry.id).arg(
                entry.name.isEmpty() ? QStringLiteral("未命名") : entry.name);
            head_->addItem(label, entry.id);
            body_->addItem(label, entry.id);
        }
    }
    QSignalBlocker headBlock(head_);
    QSignalBlocker bodyBlock(body_);
    head_->setCurrentIndex(std::max(0, head_->findData(headId)));
    body_->setCurrentIndex(std::max(0, body_->findData(bodyId)));
    status_->setText(entries.empty() ? QStringLiteral("加载模型后会自动列出类别。")
        : (headId >= 0 && headId == bodyId)
            ? QStringLiteral("头类别与身体类别不能相同，当前设置不会生效。")
            : enabled && (headId < 0 || bodyId < 0)
                ? QStringLiteral("请选择头类别和身体类别后生效。") : QString());
}

void HeadBodyFusionPage::commit() {
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.head_body_fusion_enabled = enabled_->isChecked();
        config.head_body_head_class_id = head_->currentData().toInt();
        config.head_body_body_class_id = body_->currentData().toInt();
    }
    ConfigBridge::instance().markDirty();
    refresh();
}
