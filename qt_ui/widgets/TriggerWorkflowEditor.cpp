#include "widgets/TriggerWorkflowEditor.h"
#include "config.h"
#include "mouse/am_trigger.h"
#include "widgets/FormKit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QTransform>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <functional>

namespace {
QString zh(const char* value) { return QString::fromUtf8(value); }

class DottedCanvas final : public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;
    std::function<void()> onResize;
    std::function<void(int)> onWheelZoom;
protected:
    void resizeEvent(QResizeEvent* event) override {
        QGraphicsView::resizeEvent(event);
        if (onResize) QTimer::singleShot(0, this, onResize);
    }
    void wheelEvent(QWheelEvent* event) override {
        if ((event->modifiers() & Qt::ControlModifier) && onWheelZoom) {
            onWheelZoom(event->angleDelta().y());
            event->accept();
            return;
        }
        QGraphicsView::wheelEvent(event);
    }
    void drawBackground(QPainter* painter, const QRectF& rect) override {
        painter->fillRect(rect, QColor("#151517"));
        painter->setPen(QPen(QColor("#35332D"), 1));
        const int left = static_cast<int>(rect.left()) / 20 * 20;
        const int top = static_cast<int>(rect.top()) / 20 * 20;
        for (int x = left; x < rect.right(); x += 20)
            for (int y = top; y < rect.bottom(); y += 20)
                painter->drawPoint(x, y);
    }
};

QWidget* panelPage(const QString& title, const QString& description, QVBoxLayout*& layout) {
    auto* page = new QWidget;
    layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(10);
    auto* heading = new QLabel(title);
    heading->setStyleSheet("font-size:15px; font-weight:700; color:#F0EDE6;");
    layout->addWidget(heading);
    page->setToolTip(description);
    heading->setToolTip(description);
    return page;
}
}

QSpinBox* TriggerWorkflowEditor::spin(int low, int high, int step, int value) {
    auto* result = new QSpinBox;
    result->setRange(low, high);
    result->setSingleStep(step);
    result->setValue(value);
    result->setMinimumWidth(90);
    return result;
}

TriggerWorkflowEditor::TriggerWorkflowEditor(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto* bar = new QHBoxLayout;
    enabled_ = new QCheckBox(zh(u8"启用自动扳机"));
    bar->addWidget(enabled_);
    mode_ = new QComboBox(this);
    mode_->setObjectName("triggerMode");
    mode_->addItem(zh(u8"常规扳机"), 0);
    mode_->addItem(zh(u8"正身闪打"), 1);
    mode_->addItem(zh(u8"背身闪打"), 2);
    mode_->hide();
    bar->addStretch();
    auto* zoomOut = new QPushButton(zh(u8"−"));
    zoomOut->setObjectName("workflowZoomOut");
    zoomOut->setToolTip(zh(u8"缩小流程图"));
    auto* zoomIn = new QPushButton(zh(u8"+"));
    zoomIn->setObjectName("workflowZoomIn");
    zoomIn->setToolTip(zh(u8"放大流程图"));
    zoomLabel_ = new QLabel;
    zoomLabel_->setObjectName("workflowZoomLabel");
    zoomLabel_->setMinimumWidth(48);
    zoomLabel_->setAlignment(Qt::AlignCenter);
    auto* fitButton = new QPushButton(zh(u8"适应窗口"));
    fitButton->setObjectName("workflowFitButton");
    bar->addWidget(zoomOut);
    bar->addWidget(zoomLabel_);
    bar->addWidget(zoomIn);
    bar->addWidget(fitButton);
    root->addLayout(bar);
    guide_ = new QLabel(zh(u8"上方是三种模式共用的原扳机流程；点击下方目标消失后的路线选择模式 · Ctrl + 滚轮缩放"), this);
    guide_->setVisible(false);

    snapPanel_ = new QWidget;
    auto* snapLayout = new QVBoxLayout(snapPanel_);
    snapLayout->setContentsMargins(12, 0, 12, 0);
    snapLayout->setSpacing(8);
    snapHint_ = new QLabel(snapPanel_);
    snapHint_->setWordWrap(true);
    snapHint_->setStyleSheet("color:#ABA697; font-size:12px;");
    snapHint_->setVisible(false);
    auto snapRow = [](const QString& title, QWidget* control) {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        auto* label = new QLabel(title);
        label->setMinimumWidth(215);
        label->setToolTip(control->toolTip());
        control->setFixedWidth(170);
        layout->addWidget(label);
        layout->addWidget(control);
        layout->addStretch();
        return row;
    };
    snapPixelsPerCount_ = new QDoubleSpinBox(this);
    snapPixelsPerCount_->setRange(0.05, 50.0);
    snapPixelsPerCount_->setDecimals(2);
    snapPixelsPerCount_->setSingleStep(0.05);
    snapPixelsPerCount_->setValue(1.0);
    snapPixelsPerCount_->hide();
    snapMaxCounts_ = spin(1, 500, 5, 500);
    snapCountsPerSecond_ = spin(50, 20000, 100, 4000);
    snapTolerance_ = new QDoubleSpinBox;
    snapTolerance_->setRange(1.0, 30.0);
    snapTolerance_->setDecimals(1);
    snapTolerance_->setValue(3.0);
    disappearMs_ = spin(0, 2000, 10, 80);
    disappearMs_->setObjectName("triggerFlashDisappearMs");
    disappearMs_->setToolTip(zh(u8"开火目标连续两张新画面都不见，且消失达到此时长，才开始回位或旋转。0 仍须两张新画面；设大可减少误判，但动作会变慢。"));
    snapLayout->addWidget(snapRow(zh(u8"目标消失确认 ms"), disappearMs_));
    returnSettings_ = new QWidget;
    auto* returnLayout = new QVBoxLayout(returnSettings_);
    returnLayout->setContentsMargins(0, 0, 0, 0);
    returnCountsPerSecond_ = spin(12000, 40000, 1000, 16000);
    returnCountsPerSecond_->setObjectName("triggerReturnSpeed");
    returnCountsPerSecond_->setToolTip(zh(u8"只影响回位速度：每秒最多发送多少鼠标计数。实际耗时还受驱动发送延迟影响。回位反向抵消的是程序已确认的移动，后坐力或游戏鼠标加速造成的角度变化无法仅靠计数恢复。"));
    returnLayout->addWidget(snapRow(zh(u8"回位速度 计数/秒"), returnCountsPerSecond_));
    returnYPercent_ = spin(0, 150, 5, 75);
    returnYPercent_->setObjectName("triggerReturnYPercent");
    returnYPercent_->setToolTip(zh(u8"正身回位仅对 Y 轴反向位移应用此比例，X 轴仍按原计数回位。画面回抬过高就调低，例如从 75% 调到 60%；回抬不足就调高。100% 是旧版完整反向回位。"));
    returnLayout->addWidget(snapRow(zh(u8"Y 轴回位比例 %"), returnYPercent_));
    snapLayout->addWidget(returnSettings_);
    spinSettings_ = new QWidget;
    auto* spinLayout = new QVBoxLayout(spinSettings_);
    spinLayout->setContentsMargins(0, 0, 0, 0);
    spinCountsPerTurn_ = spin(100, 200000, 500, 2400);
    spinCountsPerTurn_->setObjectName("triggerSpinCountsPerTurn");
    spinCountsPerTurn_->setToolTip(zh(u8"游戏内完整旋转 360° 所需的水平鼠标计数。你测得约 14000，就填 14000；这里不是屏幕像素，鼠标计数才能按比例换算角度。"));
    spinLayout->addWidget(snapRow(zh(u8"360° 对应鼠标计数"), spinCountsPerTurn_));
    turnStepDegrees_ = spin(1, 180, 5, 90);
    turnStepDegrees_->setObjectName("triggerTurnStepDegrees");
    turnStepDegrees_->setToolTip(zh(u8"每次快速转过的角度。每次转完才识别、瞄准和开火；无目标或目标结束后转下一段。最后一段自动截到累计 360°。"));
    spinLayout->addWidget(snapRow(zh(u8"每段转角 °"), turnStepDegrees_));
    turnDurationMs_ = spin(10, 1000, 5, 40);
    turnDurationMs_->setObjectName("triggerTurnDurationMs");
    turnDurationMs_->setToolTip(zh(u8"每段转动期望耗时。程序按此时长安排位移；硬件发送延迟过高时实际耗时可能更长。旋转过程中不进行目标识别，转完并获取新画面后才识别。"));
    spinLayout->addWidget(snapRow(zh(u8"每段转动耗时 ms"), turnDurationMs_));
    turnHoldMs_ = spin(0, 10000, 50, 300);
    turnHoldMs_->setObjectName("triggerTurnHoldMs");
    turnHoldMs_->setToolTip(zh(u8"每段转完后至少停留多久来取得新画面并识别。发现目标会立即按原扳机流程瞄准开火；每打完一个目标重新计时，在当前方向没有其他目标且停留期满后才转下一段。0 仍须至少两张新画面。"));
    spinLayout->addWidget(snapRow(zh(u8"每段停留识别 ms"), turnHoldMs_));
    // Preserve legacy fields when loading older profiles; this route no
    // longer uses either a counts-per-second limit or a turn count.
    spinTurns_ = spin(1, 10, 1, 1);
    spinTurns_->setParent(this);
    spinTurns_->hide();
    spinCountsPerSecond_ = spin(500, 200000, 5000, 5000);
    spinCountsPerSecond_->setParent(this);
    spinCountsPerSecond_->hide();
    snapLayout->addWidget(spinSettings_);
    snapLayout->addStretch();
    auto* scene = new QGraphicsScene(this);
    scene->setSceneRect(0, 0, 1600, 690);
    auto* canvas = new DottedCanvas(scene);
    canvas_ = canvas;
    canvas_->setObjectName("workflowCanvas");
    canvas_->setToolTip(guide_->text());
    canvas_->setMinimumHeight(390);
    canvas_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    canvas_->setFrameShape(QFrame::NoFrame);
    canvas_->setRenderHint(QPainter::Antialiasing);
    canvas_->setAlignment(Qt::AlignCenter);
    canvas_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    canvas_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    canvas->onResize = [this] { if (fitMode_) fitCanvas(); };
    canvas->onWheelZoom = [this](int delta) {
        if (delta != 0) zoomCanvas(delta > 0 ? 1.2 : 1.0 / 1.2);
    };
    root->addWidget(canvas_, 1);
    connect(zoomOut, &QPushButton::clicked, this, [this] { zoomCanvas(1.0 / 1.25); });
    connect(zoomIn, &QPushButton::clicked, this, [this] { zoomCanvas(1.25); });
    connect(fitButton, &QPushButton::clicked, this, [this] {
        fitMode_ = true;
        fitCanvas();
    });

    const QPen pathPen(QColor("#8E7950"), 2);
    auto arrowRight = [scene, pathPen](qreal left, qreal right, qreal y) {
        scene->addLine(left, y, right, y, pathPen);
        scene->addLine(right - 7, y - 6, right, y, pathPen);
        scene->addLine(right - 7, y + 6, right, y, pathPen);
    };
    auto* entry = new QPushButton(zh(u8"按住热键\n\n原扳机流程"));
    entry->setFixedSize(164, 100);
    entry->setEnabled(false);
    entry->setStyleSheet("QPushButton { text-align:left; padding:10px; border:2px solid #8E7950; "
                         "border-radius:10px; background:#302A1E; color:#DCD7CA; font-size:12px; font-weight:600; }");
    scene->addWidget(entry)->setPos(18, 50);
    arrowRight(182, 209, 100);

    const char* titles[] = {u8"命中区", u8"自动急停", u8"自动开镜", u8"等待开火",
                            u8"执行开火", u8"冷却", u8"3-1 切枪"};
    const char* icons[] = {"01", "02", "03", "04", "05", "06", "07"};
    for (int i = 0; i < 7; ++i) {
        const qreal x = 218 + i * 190;
        if (i > 0) arrowRight(x - 26, x - 9, 100);
        auto* node = new QPushButton;
        node->setFixedSize(164, 100);
        node->setCheckable(true);
        node->setCursor(Qt::PointingHandCursor);
        node->setProperty("nodeTitle", zh(titles[i]));
        node->setProperty("nodeNumber", QString::fromLatin1(icons[i]));
        auto* proxy = scene->addWidget(node);
        proxy->setPos(x, 50);
        nodes_[i] = node;
        connect(node, &QPushButton::clicked, this, [this, i] { selectNode(i); });
    }

    // The route choice follows the complete ordinary trigger path. Only the
    // handling after a fired target disappears differs between modes.
    scene->addLine(1522, 100, 1550, 100, pathPen);
    scene->addLine(1550, 100, 1550, 245, pathPen);
    scene->addLine(199, 245, 1550, 245, pathPen);
    scene->addLine(199, 245, 199, 550, pathPen);
    scene->addLine(760, 239, 753, 245, pathPen);
    scene->addLine(760, 251, 753, 245, pathPen);
    normalBranch_ = new QPushButton(zh(u8"常规扳机\n\n目标消失后继续"));
    normalBranch_->setFixedSize(164, 100);
    normalBranch_->setCheckable(true);
    normalBranch_->setCursor(Qt::PointingHandCursor);
    normalBranch_->setProperty("branchMode", 0);
    scene->addWidget(normalBranch_)->setPos(18, 285);
    scene->addLine(182, 335, 199, 335, pathPen);
    scene->addLine(189, 329, 182, 335, pathPen);
    scene->addLine(189, 341, 182, 335, pathPen);
    connect(normalBranch_, &QPushButton::clicked, this, [this] { selectBranch(0); });
    for (qreal y : {335.0, 550.0}) arrowRight(199, 209, y);
    const char* branchTitles[2][6] = {
        {u8"正身闪打", u8"等待目标消失", u8"结束当前射击", u8"反向回位", u8"找下一目标", u8"回到主流程"},
        {u8"背身闪打", u8"下一段转角", u8"快速转位", u8"停下识别", u8"原扳机开火", u8"累计360°停转"}
    };
    for (int lane = 0; lane < 2; ++lane) {
        const qreal y = lane == 0 ? 285 : 500;
        for (int step = 0; step < 6; ++step) {
            const qreal x = 218 + step * 190;
            if (step > 0) arrowRight(x - 26, x - 9, y + 50);
            auto* node = new QPushButton;
            node->setFixedSize(164, 100);
            node->setCheckable(true);
            node->setCursor(Qt::PointingHandCursor);
            node->setProperty("branchMode", lane + 1);
            node->setProperty("branchStep", step);
            node->setProperty("branchTitle", zh(branchTitles[lane][step]));
            scene->addWidget(node)->setPos(x, y);
            branchNodes_[lane][step] = node;
            connect(node, &QPushButton::clicked, this,
                    [this, lane] { selectBranch(lane + 1); });
        }
    }

    inspector_ = new QStackedWidget;
    auto* detailScroll = new QScrollArea;
    classicDetail_ = detailScroll;
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setMinimumHeight(210);
    detailScroll->setMaximumHeight(260);
    detailScroll->setWidget(inspector_);
    root->addWidget(detailScroll);
    root->addWidget(snapPanel_);

    QVBoxLayout* page = nullptr;
    auto add = [this](QWidget* widget) { inspector_->addWidget(widget); };
    auto row = [](QVBoxLayout* layout, const char* label, QWidget* input) {
        layout->addWidget(FormKit::fieldRow(zh(label), input));
    };

    auto* zonePage = panelPage(zh(u8"01 · 命中区"),
        zh(u8"未设置独立扳机类别时使用此旧版范围；独立类别的瞄点和宽高在下方卡片设置。"), page);
    zone_ = spin(10, 1000, 5, 100); row(page, u8"区域比例 %", zone_);
    page->addStretch(); add(zonePage);

    auto* stopPage = panelPage(zh(u8"02 · 自动急停"),
        zh(u8"MAKCUNEW 可屏蔽真实键盘；KMBoxNet 屏蔽 W/A/S/D。没有对应键盘能力时跳过。"), page);
    stopMode_ = new QComboBox;
    stopMode_->addItem(zh(u8"跳过"), 0);
    stopMode_->addItem(zh(u8"按开枪时间屏蔽"), 1);
    stopMode_->addItem(zh(u8"进入命中区持续屏蔽"), 2);
    row(page, u8"方式", stopMode_);
    stopBefore_ = spin(0, 1000, 10, 0); row(page, u8"开枪前 ms", stopBefore_);
    stopAfter_ = spin(0, 1000, 10, 60); row(page, u8"开枪后 ms", stopAfter_);
    page->addStretch(); add(stopPage);

    auto* scopePage = panelPage(zh(u8"03 · 自动开镜"),
        zh(u8"右键作为热键时不重复开镜。点按右键至少保持 30 ms，采用按住时长与统一随机浮动；长按右键当次即可开火，直到停用才松开。"), page);
    scopeMode_ = new QComboBox;
    scopeMode_->addItem(zh(u8"跳过"), 0);
    scopeMode_->addItem(zh(u8"点按开镜（切枪后重新开镜）"), 1);
    scopeMode_->addItem(zh(u8"触发后长按"), 2);
    row(page, u8"方式", scopeMode_);
    scopeDelay_ = spin(0, 2000, 5, 0); scopeDelay_->setParent(this); scopeDelay_->hide();
    page->addStretch(); add(scopePage);

    auto* waitPage = panelPage(zh(u8"04 · 等待开火"),
        zh(u8"AM 规则：进入射击区域后开始计时，换目标不重启计时。智能连点跳过起始等待；基础时长为 0 时不增加随机等待。"), page);
    fireDelay_ = spin(0, 2000, 5, 0); row(page, u8"起始等待 ms", fireDelay_);
    delayJitter_ = spin(0, 500, 1, 0); row(page, u8"统一随机浮动 ±ms", delayJitter_);
    prearmEnabled_ = new QCheckBox(zh(u8"预备开火"));
    prearmEnabled_->setObjectName("triggerPrearmEnabled");
    prearmEnabled_->setToolTip(zh(u8"目标靠近命中区时先累计首发等待；长按开镜也提前开始。真正进入命中区且开镜就绪后才按左键。点按开镜仍在进入命中区后执行。"));
    prearmEnabled_->setParent(this); prearmEnabled_->hide();
    prearmExpand_ = spin(0, 300, 5, 50);
    prearmExpand_->setObjectName("triggerPrearmExpand");
    prearmExpand_->setToolTip(zh(u8"相对当前扳机命中框向四周扩展的比例。50% 表示预备框的半宽和半高各增加 50%，不改变真正开火范围。"));
    prearmExpand_->setParent(this); prearmExpand_->hide();
    page->addStretch(); add(waitPage);

    auto* firePage = panelPage(zh(u8"05 · 执行开火"),
        zh(u8"智能连点：进区即按下，离区即停止。连点：起始等待后循环，按住时长按设置执行（最小 1 ms）。持续：首次触发后保持到停用。智能持续：离区超过宽限才松开。3-1 切枪使用短按。"), page);
    fireMode_ = new QComboBox;
    fireMode_->setObjectName("triggerFireMode");
    fireMode_->addItem(zh(u8"智能连点"), 0);
    fireMode_->addItem(zh(u8"连点"), 1);
    fireMode_->addItem(zh(u8"持续"), 2);
    fireMode_->addItem(zh(u8"智能持续"), 3);
    row(page, u8"开火模式", fireMode_);
    fireDuration_ = spin(0, 2000, 5, 0); row(page, u8"按住时长 ms", fireDuration_);
    durationJitter_ = spin(0, 500, 1, 0); durationJitter_->setParent(this); durationJitter_->hide();
    lossDelay_ = spin(0, 1000, 10, 20);
    lossDelay_->setObjectName("triggerLossDelay");
    lossDelay_->setToolTip(zh(u8"仅智能持续生效：短暂丢失或离区时保持左键，到时松开；任一允许目标重新进区就重置计时。0 为立即松开；松开热键或关闭扳机立即停止。"));
    row(page, u8"短暂离区宽限 ms", lossDelay_);
    page->addStretch(); add(firePage);

    auto* cooldownPage = panelPage(zh(u8"06 · 冷却"),
        zh(u8"从松开左键开始按设置的时长计时（最小 1 ms），到时经过一轮状态清理再允许下次按下。"), page);
    fireInterval_ = spin(0, 2000, 5, 200); row(page, u8"松开后等待 ms", fireInterval_);
    intervalJitter_ = spin(0, 500, 1, 0); intervalJitter_->setParent(this); intervalJitter_->hide();
    targetCooldown_ = spin(0, 2000, 5, 0); targetCooldown_->setParent(this); targetCooldown_->hide();
    page->addStretch(); add(cooldownPage);

    auto* switchPage = panelPage(zh(u8"07 · 3-1 切枪"),
        zh(u8"只有左键按下与松开都成功，且按住足够久，才发送 3 → 1 → 1 → 1。按键时长与间隔固定 20 ms。"), page);
    switchEnabled_ = new QCheckBox(zh(u8"开火后执行切枪"));
    page->addWidget(switchEnabled_);
    switchDelay_ = spin(0, 2000, 5, 50); row(page, u8"开火后等待 ms", switchDelay_);
    page->addStretch(); add(switchPage);

    auto onChange = [this] { changed(); };
    connect(enabled_, &QCheckBox::toggled, this, onChange);
    connect(mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onChange);
    connect(snapPixelsPerCount_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onChange);
    connect(snapTolerance_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onChange);
    for (auto* input : {snapMaxCounts_, snapCountsPerSecond_, returnCountsPerSecond_,
                        returnYPercent_,
                        disappearMs_, spinCountsPerTurn_,
                        turnStepDegrees_, turnDurationMs_, turnHoldMs_})
        connect(input, QOverload<int>::of(&QSpinBox::valueChanged), this, onChange);
    connect(switchEnabled_, &QCheckBox::toggled, this, onChange);
    connect(prearmEnabled_, &QCheckBox::toggled, this, onChange);
    for (auto* combo : {stopMode_, scopeMode_, fireMode_})
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onChange);
    for (auto* input : {zone_, targetCooldown_, stopBefore_, stopAfter_, scopeDelay_,
                        fireDelay_, delayJitter_, prearmExpand_, fireDuration_, durationJitter_, lossDelay_,
                        fireInterval_, intervalJitter_, switchDelay_})
        connect(input, QOverload<int>::of(&QSpinBox::valueChanged), this, onChange);
    selectNode(0);
    refreshNodes();
    zoomLabel_->setText(zh(u8"自适应"));
}

void TriggerWorkflowEditor::fitCanvas() {
    if (!canvas_ || canvas_->viewport()->width() <= 0) return;
    canvas_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    canvas_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    canvas_->fitInView(canvas_->scene()->sceneRect(), Qt::KeepAspectRatio);
    zoom_ = canvas_->transform().m11();
    zoomLabel_->setText(QString::number(qRound(zoom_ * 100)) + "%");
}

void TriggerWorkflowEditor::zoomCanvas(qreal factor) {
    if (!canvas_) return;
    const QPointF center = canvas_->mapToScene(canvas_->viewport()->rect().center());
    zoom_ = std::clamp(zoom_ * factor, 0.2, 2.0);
    fitMode_ = false;
    canvas_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    canvas_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    canvas_->setTransform(QTransform::fromScale(zoom_, zoom_));
    canvas_->centerOn(center);
    zoomLabel_->setText(QString::number(qRound(zoom_ * 100)) + "%");
}

void TriggerWorkflowEditor::selectNode(int index) {
    detailBranchSelected_ = false;
    inspector_->setCurrentIndex(index);
    refreshNodes();
}

void TriggerWorkflowEditor::selectBranch(int mode) {
    detailBranchSelected_ = mode != 0;
    const int index = mode_->findData(mode);
    if (index >= 0) mode_->setCurrentIndex(index);
    refreshNodes();
}

void TriggerWorkflowEditor::changed() {
    refreshNodes();
    if (!loading_ && changed_) changed_();
}

void TriggerWorkflowEditor::refreshNodes() {
    const int branch = mode_->currentData().toInt();
    const bool snapMode = branch != 0;
    snapPanel_->setVisible(snapMode && detailBranchSelected_);
    spinSettings_->setVisible(branch == 2);
    returnSettings_->setVisible(branch == 1);
    snapHint_->setText(branch == 2
        ? zh(u8"背身闪打：转一段设定角度后停下，在该方向停留识别并按原扳机流程处理目标。每打完一个目标重新计时，无其他目标且停留期满才转下一段。一次按住热键累计最多转 360°；松键立即停止。")
        : zh(u8"正身闪打：先完整执行上方原扳机流程。已开火目标持续消失后，X 轴按确认位移反向回位，Y 轴按所设比例回拉，再处理下一目标。画面无法直接确认击杀；后坐力或鼠标加速可能造成回位误差。松键立即停止。"));
    snapPanel_->setToolTip(snapHint_->text());
    classicDetail_->setVisible(!detailBranchSelected_);
    const QString summaries[] = {
        hasClassRules_ ? zh(u8"逐类别命中区")
                       : zh(u8"命中区 ") + QString::number(zone_->value()) + "%",
        stopMode_->currentIndex() == 0 ? zh(u8"跳过") : stopMode_->currentText(),
        scopeMode_->currentIndex() == 0 ? zh(u8"跳过") : scopeMode_->currentText(),
        QString::number(fireDelay_->value()) + " ms" +
            (prearmEnabled_->isChecked() ? zh(u8" · 预备") : QString()),
        fireMode_->currentText(),
        QString::number(fireInterval_->value()) + " ms",
        switchEnabled_->isChecked() ? zh(u8"开火后执行") : zh(u8"跳过")
    };
    for (int i = 0; i < 7; ++i) {
        auto* node = nodes_[i];
        node->setText(node->property("nodeNumber").toString() + "   " +
                      node->property("nodeTitle").toString() + "\n\n" + summaries[i]);
        const bool selected = inspector_->currentIndex() == i;
        node->setChecked(selected);
        node->setToolTip(inspector_->widget(i)->toolTip());
        const QString border = selected ? "#D5B56B" : "#514C40";
        const QString bg = selected ? "#302A1E" : "#19191C";
        node->setStyleSheet(QString(
            "QPushButton { text-align:left; padding:10px; border:2px solid %1; "
            "border-radius:10px; background:%2; color:#DCD7CA; font-size:12px; font-weight:600; }"
            "QPushButton:hover { border-color:#E9CD8A; }").arg(border, bg));
    }
    const QString returnSummaries[] = {
        zh(u8"目标结束路线"), zh(u8"新帧确认"), zh(u8"按原流程松键"),
        QString::number(returnCountsPerSecond_->value()) + zh(u8" 计数/秒") +
            "\nY " + QString::number(returnYPercent_->value()) + "%",
        zh(u8"立即继续"), zh(u8"下一目标")
    };
    const QString spinSummaries[] = {
        zh(u8"目标结束路线"), QString::number(turnStepDegrees_->value()) + zh(u8"° / 段"),
        QString::number(turnDurationMs_->value()) + " ms", QString::number(turnHoldMs_->value()) + zh(u8" ms 停留"),
        zh(u8"上方主流程"), zh(u8"停转仍识别")
    };
    normalBranch_->setChecked(branch == 0);
    normalBranch_->setToolTip(zh(u8"原扳机流程直接寻找下一目标，不进行回位或旋转搜索。"));
    normalBranch_->setStyleSheet(QString(
        "QPushButton { text-align:left; padding:10px; border:2px solid %1; "
        "border-radius:10px; background:%2; color:#DCD7CA; font-size:12px; font-weight:600; }"
        "QPushButton:hover { border-color:#E9CD8A; }")
        .arg(branch == 0 ? "#D5B56B" : "#514C40", branch == 0 ? "#302A1E" : "#242321"));
    for (int lane = 0; lane < 2; ++lane) {
        const bool active = branch == lane + 1;
        for (int step = 0; step < 6; ++step) {
            auto* node = branchNodes_[lane][step];
            const QString summary = lane == 0 ? returnSummaries[step] : spinSummaries[step];
            node->setText(node->property("branchTitle").toString() + "\n\n" + summary);
            node->setChecked(active && step == 0);
            node->setToolTip(lane == 0
                ? zh(u8"先执行上方原扳机流程。已开火目标持续消失后，X 全量反向、Y 按比例回拉，再返回原流程。无法从画面直接确认击杀。")
                : zh(u8"每段快速转完才取新画面，找到目标按上方原扳机流程瞄准开火。目标结束后转下一段，一次热键累计最多 360°。"));
            node->setStyleSheet(QString(
                "QPushButton { text-align:left; padding:10px; border:2px solid %1; "
                "border-radius:10px; background:%2; color:#DCD7CA; font-size:12px; font-weight:600; }"
                "QPushButton:hover { border-color:#E9CD8A; }")
                .arg(active ? "#D5B56B" : "#514C40", active ? "#302A1E" : "#242321"));
        }
    }
    stopBefore_->setEnabled(stopMode_->currentData().toInt() == 1);
    stopAfter_->setEnabled(stopMode_->currentData().toInt() == 1);
    scopeDelay_->setEnabled(scopeMode_->currentData().toInt() != 0);
    switchDelay_->setEnabled(switchEnabled_->isChecked());
    lossDelay_->setEnabled(fireMode_->currentData().toInt() == 3 && !switchEnabled_->isChecked());
    prearmEnabled_->setEnabled(false);
    prearmExpand_->setEnabled(false);
    prearmEnabled_->setToolTip(zh(u8"AM 只在进入射击区域后开始等待，不提前累计。"));
    targetCooldown_->setEnabled(false);
    targetCooldown_->setToolTip(zh(u8"AM 不因目标编号变化额外等待。"));
    scopeDelay_->setEnabled(false);
    scopeDelay_->setToolTip(zh(u8"点按开镜采用按住时长，至少 30 ms；长按不额外等待。"));
    durationJitter_->setEnabled(false);
    intervalJitter_->setEnabled(false);
    fireDelay_->setEnabled(fireMode_->currentData().toInt() != 0);
    fireInterval_->setEnabled(fireMode_->currentData().toInt() < 2);
    zone_->setEnabled(!hasClassRules_);
}

void TriggerWorkflowEditor::load(const HotkeyProfile& p) {
    load(triggerParamsOf(p));
    hasClassRules_ = !p.trigger_classes.empty();
    refreshNodes();
}

void TriggerWorkflowEditor::save(HotkeyProfile& p) const {
    TriggerParams params;
    save(params);
    applyTriggerParams(p, params);
}

void TriggerWorkflowEditor::load(const TriggerParams& p) {
    loading_ = true;
    hasClassRules_ = false;
    enabled_->setChecked(p.trigger_enabled);
    mode_->setCurrentIndex(std::max(0, mode_->findData(p.trigger_mode)));
    snapPixelsPerCount_->setValue(p.trigger_snap_px_per_count);
    snapMaxCounts_->setValue(p.trigger_snap_max_counts);
    snapTolerance_->setValue(p.trigger_snap_tolerance_px);
    snapCountsPerSecond_->setValue(p.trigger_snap_counts_per_second);
    returnCountsPerSecond_->setValue(p.trigger_return_counts_per_second);
    returnYPercent_->setValue(p.trigger_return_y_percent);
    spinCountsPerTurn_->setValue(p.trigger_spin_counts_per_turn);
    turnStepDegrees_->setValue(p.trigger_spin_step_degrees);
    turnDurationMs_->setValue(p.trigger_spin_step_ms);
    turnHoldMs_->setValue(p.trigger_spin_hold_ms);
    spinCountsPerSecond_->setValue(p.trigger_spin_counts_per_second);
    spinTurns_->setValue(p.trigger_spin_turns);
    legacySnapHoldMs_ = p.trigger_snap_fire_hold_ms;
    legacyCooldownMs_ = p.trigger_snap_cooldown_ms;
    disappearMs_->setValue(p.trigger_flash_disappear_ms);
    detailBranchSelected_ = p.trigger_mode != 0;
    zone_->setValue(p.trigger_y_percent);
    targetCooldown_->setValue(p.trigger_switch_cooldown_ms);
    stopMode_->setCurrentIndex(std::max(0, stopMode_->findData(p.trigger_auto_stop)));
    stopBefore_->setValue(p.trigger_stop_before_ms);
    stopAfter_->setValue(p.trigger_stop_after_ms);
    scopeMode_->setCurrentIndex(std::max(0, scopeMode_->findData(p.trigger_auto_scope)));
    scopeDelay_->setValue(p.trigger_scope_delay_ms);
    fireDelay_->setValue(p.trigger_fire_delay);
    prearmEnabled_->setChecked(false);
    prearmExpand_->setValue(p.trigger_prearm_expand_percent);
    delayJitter_->setValue(p.trigger_delay_jitter_ms);
    fireMode_->setCurrentIndex(static_cast<int>(boss::amFireMode(p.trigger_fire_mode, p.trigger_fire_duration)));
    fireDuration_->setValue(p.trigger_fire_duration);
    lossDelay_->setValue(p.trigger_loss_delay_ms);
    durationJitter_->setValue(p.trigger_duration_jitter_ms);
    fireInterval_->setValue(p.trigger_fire_interval);
    intervalJitter_->setValue(p.trigger_interval_jitter_ms);
    switchEnabled_->setChecked(p.trigger_weapon_switch31);
    switchDelay_->setValue(p.trigger_switch31_delay_ms);
    loading_ = false;
    refreshNodes();
}

void TriggerWorkflowEditor::save(TriggerParams& p) const {
    p.trigger_enabled = enabled_->isChecked();
    p.trigger_mode = mode_->currentData().toInt();
    p.trigger_snap_px_per_count = snapPixelsPerCount_->value();
    p.trigger_snap_max_counts = snapMaxCounts_->value();
    p.trigger_snap_tolerance_px = snapTolerance_->value();
    p.trigger_snap_counts_per_second = snapCountsPerSecond_->value();
    p.trigger_return_counts_per_second = returnCountsPerSecond_->value();
    p.trigger_return_y_percent = returnYPercent_->value();
    p.trigger_spin_counts_per_turn = spinCountsPerTurn_->value();
    p.trigger_spin_step_degrees = turnStepDegrees_->value();
    p.trigger_spin_step_ms = turnDurationMs_->value();
    p.trigger_spin_hold_ms = turnHoldMs_->value();
    p.trigger_spin_counts_per_second = spinCountsPerSecond_->value();
    p.trigger_spin_turns = spinTurns_->value();
    p.trigger_snap_fire_hold_ms = legacySnapHoldMs_;
    p.trigger_snap_cooldown_ms = legacyCooldownMs_;
    p.trigger_flash_disappear_ms = disappearMs_->value();
    p.trigger_y_percent = zone_->value();
    p.trigger_switch_cooldown_ms = 0;
    p.trigger_auto_stop = stopMode_->currentData().toInt();
    p.trigger_stop_before_ms = stopBefore_->value();
    p.trigger_stop_after_ms = stopAfter_->value();
    p.trigger_auto_scope = scopeMode_->currentData().toInt();
    p.trigger_scope_delay_ms = scopeDelay_->value();
    p.trigger_fire_delay = fireDelay_->value();
    p.trigger_prearm_enabled = false;
    p.trigger_prearm_expand_percent = prearmExpand_->value();
    p.trigger_delay_jitter_ms = delayJitter_->value();
    p.trigger_fire_mode = fireMode_->currentData().toInt();
    p.trigger_fire_duration = fireDuration_->value();
    p.trigger_loss_delay_ms = lossDelay_->value();
    p.trigger_duration_jitter_ms = delayJitter_->value();
    p.trigger_fire_interval = fireInterval_->value();
    p.trigger_interval_jitter_ms = delayJitter_->value();
    p.trigger_weapon_switch31 = switchEnabled_->isChecked();
    p.trigger_switch31_delay_ms = switchDelay_->value();
}
