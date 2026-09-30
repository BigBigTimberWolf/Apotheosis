#include "widgets/TriggerTargetEditor.h"

#include "widgets/FormKit.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

class ZonePreview final : public QWidget {
public:
    explicit ZonePreview(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(220);
    }
    void setRule(const TriggerAimClass& rule) {
        rule_ = rule;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor("#F8FAFD"));
        const double px = 100.0 * rule_.x_offset;
        const double py = 160.0 * (1.0 - rule_.y_offset);
        const double halfX = rule_.range_x_percent * 0.5;
        const double halfY = rule_.range_y_percent * 0.8;
        const double minX = std::min(0.0, px - halfX);
        const double maxX = std::max(100.0, px + halfX);
        const double minY = std::min(0.0, py - halfY);
        const double maxY = std::max(160.0, py + halfY);
        const double scale = std::min((width() - 42.0) / std::max(1.0, maxX - minX),
                                      (height() - 42.0) / std::max(1.0, maxY - minY));
        const double ox = (width() - (maxX - minX) * scale) * 0.5 - minX * scale;
        const double oy = (height() - (maxY - minY) * scale) * 0.5 - minY * scale;
        const auto rectAt = [&](double x, double y, double w, double h) {
            return QRectF(ox + x * scale, oy + y * scale, w * scale, h * scale);
        };
        p.setPen(QPen(QColor("#64748B"), 2));
        p.setBrush(QColor("#FFFFFF"));
        p.drawRect(rectAt(0, 0, 100, 160));
        p.setPen(QPen(QColor("#D946A4"), 2));
        p.setBrush(QColor(217, 70, 164, 48));
        p.drawRect(rectAt(px - halfX, py - halfY, halfX * 2, halfY * 2));
        p.setPen(QPen(QColor("#D946A4"), 2));
        p.drawLine(QPointF(ox + px * scale - 6, oy + py * scale),
                   QPointF(ox + px * scale + 6, oy + py * scale));
        p.drawLine(QPointF(ox + px * scale, oy + py * scale - 6),
                   QPointF(ox + px * scale, oy + py * scale + 6));
    }
private:
    TriggerAimClass rule_{};
};

} // namespace

TriggerTargetEditor::TriggerTargetEditor(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);
    const QString hint = QStringLiteral(
        "这里的类别、顺序和瞄点独立于自动瞄准。列表越靠上优先级越高；"
        "空列表沿用旧版扳机命中区。范围宽高按目标框百分比计算，可超过目标框。");
    setToolTip(hint);

    auto* addRow = new QHBoxLayout;
    addClass_ = new QComboBox;
    addClass_->setObjectName("triggerAddClass");
    addClass_->setToolTip(hint);
    auto* addButton = new QPushButton(QStringLiteral("添加类别"));
    addRow->addWidget(addClass_, 1);
    addRow->addWidget(addButton);
    root->addLayout(addRow);

    list_ = new QListWidget;
    list_->setObjectName("triggerClassList");
    list_->setToolTip(hint);
    list_->setMinimumHeight(120);
    root->addWidget(list_);
    auto* orderRow = new QHBoxLayout;
    auto* up = new QPushButton(QStringLiteral("上移优先级"));
    auto* down = new QPushButton(QStringLiteral("下移优先级"));
    auto* remove = new QPushButton(QStringLiteral("移除类别"));
    orderRow->addWidget(up);
    orderRow->addWidget(down);
    orderRow->addWidget(remove);
    orderRow->addStretch();
    root->addLayout(orderRow);

    pointX_ = new QDoubleSpinBox;
    pointY_ = new QDoubleSpinBox;
    for (auto* s : {pointX_, pointY_}) {
        s->setRange(0.0, 1.0);
        s->setSingleStep(0.05);
        s->setDecimals(2);
    }
    pointX_->setObjectName("triggerPointX");
    pointY_->setObjectName("triggerPointY");
    root->addWidget(FormKit::fieldRow(QStringLiteral("扳机瞄点 X（0 左，1 右）"), pointX_));
    root->addWidget(FormKit::fieldRow(QStringLiteral("扳机瞄点 Y（0 下，1 上）"), pointY_));
    rangeX_ = new QSpinBox;
    rangeY_ = new QSpinBox;
    for (auto* s : {rangeX_, rangeY_}) {
        s->setRange(10, 1000);
        s->setSingleStep(5);
        s->setSuffix(QStringLiteral(" %"));
    }
    rangeX_->setObjectName("triggerRangeX");
    rangeY_->setObjectName("triggerRangeY");
    root->addWidget(FormKit::fieldRow(QStringLiteral("范围宽度（相对框宽）"), rangeX_));
    root->addWidget(FormKit::fieldRow(QStringLiteral("范围高度（相对框高）"), rangeY_));
    sizeLabel_ = new QLabel;
    root->addWidget(sizeLabel_);
    preview_ = new ZonePreview;
    preview_->setObjectName("triggerZonePreview");
    preview_->setToolTip(QStringLiteral("灰框：目标框；粉框：实际扳机范围；十字：扳机瞄点。"));
    root->addWidget(preview_);

    connect(addButton, &QPushButton::clicked, this, [this] {
        const int id = addClass_->currentData().toInt();
        if (addClass_->currentIndex() < 0 || id < 0 ||
            std::any_of(classes_.begin(), classes_.end(), [id](const auto& c) { return c.class_id == id; })) return;
        TriggerAimClass rule;
        rule.class_id = id;
        rule.range_x_percent = rule.range_y_percent = defaultRange_;
        classes_.push_back(rule);
        rebuildList(static_cast<int>(classes_.size()) - 1);
        notifyChanged();
    });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { refreshFields(); });
    connect(up, &QPushButton::clicked, this, [this] {
        const int i = list_->currentRow();
        if (i <= 0) return;
        std::swap(classes_[i], classes_[i - 1]);
        rebuildList(i - 1);
        notifyChanged();
    });
    connect(down, &QPushButton::clicked, this, [this] {
        const int i = list_->currentRow();
        if (i < 0 || i + 1 >= static_cast<int>(classes_.size())) return;
        std::swap(classes_[i], classes_[i + 1]);
        rebuildList(i + 1);
        notifyChanged();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int i = list_->currentRow();
        if (i < 0 || i >= static_cast<int>(classes_.size())) return;
        classes_.erase(classes_.begin() + i);
        rebuildList(std::min(i, static_cast<int>(classes_.size()) - 1));
        notifyChanged();
    });
    for (auto* s : {pointX_, pointY_})
        connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this] { editSelected(); });
    for (auto* s : {rangeX_, rangeY_})
        connect(s, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this] { editSelected(); });
    refreshFields();
}

QString TriggerTargetEditor::className(int id) const {
    for (const auto& entry : available_)
        if (entry.first == id) return entry.second;
    return QStringLiteral("类别 %1").arg(id);
}

void TriggerTargetEditor::load(const HotkeyProfile& profile,
                               const std::vector<ClassFilterState>& available) {
    loading_ = true;
    classes_ = profile.trigger_classes;
    defaultRange_ = std::clamp(profile.trigger_y_percent, 10, 1000);
    available_.clear();
    for (const auto& c : available)
        available_.emplace_back(c.class_id, QString::fromStdString(c.class_name));
    for (const auto& c : classes_)
        if (std::none_of(available_.begin(), available_.end(), [&](const auto& entry) {
            return entry.first == c.class_id;
        })) available_.emplace_back(c.class_id, QStringLiteral("已保存类别"));
    {
        QSignalBlocker blocker(addClass_);
        addClass_->clear();
        for (const auto& entry : available_)
            addClass_->addItem(QStringLiteral("[%1] %2").arg(entry.first).arg(entry.second), entry.first);
    }
    rebuildList(classes_.empty() ? -1 : 0);
    loading_ = false;
}

void TriggerTargetEditor::rebuildList(int selected) {
    QSignalBlocker blocker(list_);
    list_->clear();
    for (size_t i = 0; i < classes_.size(); ++i) {
        const auto& c = classes_[i];
        list_->addItem(QStringLiteral("%1. [%2] %3  ·  X %4% × Y %5%")
            .arg(static_cast<int>(i + 1)).arg(c.class_id).arg(className(c.class_id))
            .arg(c.range_x_percent).arg(c.range_y_percent));
    }
    list_->setCurrentRow(selected);
    refreshFields();
}

void TriggerTargetEditor::refreshFields() {
    const int i = list_->currentRow();
    const bool valid = i >= 0 && i < static_cast<int>(classes_.size());
    for (auto* s : {pointX_, pointY_}) s->setEnabled(valid);
    for (auto* s : {rangeX_, rangeY_}) s->setEnabled(valid);
    sizeLabel_->setText(valid ? QStringLiteral(
        "目标框若为 100 × 160 px，当前扳机范围为 %1 × %2 px（宽 × 高）。")
            .arg(classes_[i].range_x_percent)
            .arg(classes_[i].range_y_percent * 1.6, 0, 'f', 1)
        : QStringLiteral("添加并选择一个类别后，可设置该类别的瞄点和范围。"));
    if (!valid) { preview_->hide(); return; }
    QSignalBlocker bx(pointX_), by(pointY_), bw(rangeX_), bh(rangeY_);
    const auto& c = classes_[i];
    pointX_->setValue(c.x_offset);
    pointY_->setValue(c.y_offset);
    rangeX_->setValue(c.range_x_percent);
    rangeY_->setValue(c.range_y_percent);
    static_cast<ZonePreview*>(preview_)->setRule(c);
    preview_->show();
}

void TriggerTargetEditor::editSelected() {
    if (loading_) return;
    const int i = list_->currentRow();
    if (i < 0 || i >= static_cast<int>(classes_.size())) return;
    auto& c = classes_[i];
    c.x_offset = static_cast<float>(pointX_->value());
    c.y_offset = static_cast<float>(pointY_->value());
    c.range_x_percent = rangeX_->value();
    c.range_y_percent = rangeY_->value();
    rebuildList(i);
    notifyChanged();
}

void TriggerTargetEditor::notifyChanged() {
    if (!loading_ && changed_) changed_();
}
