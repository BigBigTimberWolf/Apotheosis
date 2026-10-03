#include "widgets/NeuralCurveTrainer.h"

#include "widgets/CurveCanvas.h"
#include "widgets/XmCurveImport.h"

#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <exception>
#include <thread>
#include <utility>

namespace {
constexpr int kPadding = 28;
constexpr double kStartRadius = 18.0;
constexpr double kTargetRadius = 15.0;
}

NeuralCurveTrainingCanvas::NeuralCurveTrainingCanvas(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(590, 340);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
}

void NeuralCurveTrainingCanvas::startRecording(int rounds, bool append)
{
    requested_ = std::clamp(rounds, 5, 200);
    acceptedThisRun_ = 0;
    rejected_ = 0;
    rejectionReason_.clear();
    if (!append) { trajectories_.clear(); importedCount_ = 0; }
    recording_ = true;
    beginRound();
}

void NeuralCurveTrainingCanvas::addImportedTrajectories(std::vector<boss::NeuralTrajectory> imported)
{
    const size_t count = imported.size();
    trajectories_.insert(trajectories_.begin() + static_cast<std::ptrdiff_t>(importedCount_),
                         std::make_move_iterator(imported.begin()),
                         std::make_move_iterator(imported.end()));
    importedCount_ += count;
}

void NeuralCurveTrainingCanvas::stopRecording()
{
    recording_ = false;
    dragging_ = false;
    stroke_.clear();
    update();
}

void NeuralCurveTrainingCanvas::beginRound()
{
    start_ = rect().center();
    const QRectF area = QRectF(rect()).adjusted(kPadding, kPadding, -kPadding, -kPadding);
    const double minDistance = std::min(area.width(), area.height()) * 0.35;
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        target_ = QPointF(area.left() + QRandomGenerator::global()->generateDouble() * area.width(),
                          area.top() + QRandomGenerator::global()->generateDouble() * area.height());
        if (QLineF(start_, target_).length() >= minDistance) break;
    }
    dragging_ = false;
    stroke_.clear();
    if (progressChanged)
        progressChanged(acceptedThisRun_, requested_, rejected_);
    update();
}

boss::NeuralTrajectory NeuralCurveTrainingCanvas::normalizeStroke(QString& reason) const
{
    // The normalization itself lives in neural_curve.h so that live recordings and
    // imported files are judged by exactly the same rules.
    std::vector<boss::NeuralPoint2D> stroke;
    stroke.reserve(stroke_.size());
    for (const QPointF& point : stroke_) stroke.push_back({point.x(), point.y()});
    std::string why;
    auto trajectory = boss::normalizeNeuralStroke(
        {start_.x(), start_.y()}, {target_.x(), target_.y()}, kTargetRadius, stroke, why);
    if (trajectory.empty()) reason = QString::fromUtf8(why.c_str());
    return trajectory;
}

void NeuralCurveTrainingCanvas::finishRound()
{
    rejectionReason_.clear();
    auto normalized = normalizeStroke(rejectionReason_);
    if (!normalized.empty())
    {
        // Only live recordings are windowed to the latest 200; imports stay.
        if (trajectories_.size() - importedCount_ >= 200)
            trajectories_.erase(trajectories_.begin() + static_cast<std::ptrdiff_t>(importedCount_));
        trajectories_.push_back(std::move(normalized));
        ++acceptedThisRun_;
    }
    else ++rejected_;
    if (acceptedThisRun_ >= requested_)
    {
        stopRecording();
        if (progressChanged) progressChanged(acceptedThisRun_, requested_, rejected_);
        if (collectionFinished) collectionFinished();
    }
    else beginRound();
}

void NeuralCurveTrainingCanvas::mousePressEvent(QMouseEvent* event)
{
    if (!recording_ || event->button() != Qt::LeftButton ||
        QLineF(event->position(), start_).length() > kStartRadius)
        return;
    dragging_ = true;
    stroke_ = { start_, event->position() };
    update();
}

void NeuralCurveTrainingCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!recording_ || !dragging_) return;
    appendStroke(event->position());
}

void NeuralCurveTrainingCanvas::appendStroke(QPointF point)
{
    // Desktop mouse events can skip across the whole target between samples.
    // Test the segment, not just its final pixel, and keep the arrival point.
    const QPointF previous = stroke_.empty() ? start_ : stroke_.back();
    const QPointF step = point - previous;
    const double length2 = QPointF::dotProduct(step, step);
    const double fraction = length2 > 0.0
        ? std::clamp(QPointF::dotProduct(target_ - previous, step) / length2, 0.0, 1.0)
        : 0.0;
    const QPointF closest = previous + step * fraction;
    const bool arrived = QLineF(closest, target_).length() <= kTargetRadius;
    if (arrived) point = closest;
    if (stroke_.empty() || QLineF(stroke_.back(), point).length() >= 1.0)
        stroke_.push_back(point);
    if (arrived)
        finishRound();
    else update();
}

void NeuralCurveTrainingCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (!recording_ || !dragging_ || event->button() != Qt::LeftButton) return;
    appendStroke(event->position());
    if (!recording_ || !dragging_) return; // Arrival already finished this round.
    dragging_ = false;
    ++rejected_;
    rejectionReason_ = QString::fromUtf8(u8"尚未到达绿色目标就松开，请重录本条");
    beginRound();
}

void NeuralCurveTrainingCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#151517"));
    p.setPen(QPen(QColor("#35332D"), 1));
    p.setBrush(QColor("#19191C"));
    p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 10, 10);
    if (!recording_)
    {
        p.setPen(QColor("#ABA697"));
        p.drawText(rect(), Qt::AlignCenter,
                   QString::fromUtf8(u8"设置轮数并开始录制\n从金色起点按住左键拖向绿色目标"));
        return;
    }
    if (stroke_.size() >= 2)
    {
        QPainterPath path(stroke_.front());
        for (size_t i = 1; i < stroke_.size(); ++i) path.lineTo(stroke_[i]);
        p.setPen(QPen(QColor(213, 181, 107, 160), 2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
    p.setPen(QPen(QColor("#D5B56B"), 2));
    p.setBrush(QColor("#E9CD8A"));
    p.drawEllipse(start_, kStartRadius, kStartRadius);
    p.setPen(QPen(QColor("#53C583"), 2));
    p.setBrush(QColor("#22C55E"));
    p.drawEllipse(target_, kTargetRadius, kTargetRadius);
    p.setPen(QColor("#BCB7AA"));
    p.drawText(QRectF(12, 8, width() - 24, 25), Qt::AlignLeft,
               QString::fromUtf8(u8"第 %1 / %2 条（无效重录 %3）")
                   .arg(acceptedThisRun_ + 1).arg(requested_).arg(rejected_));
}

NeuralCurveTrainerDialog::NeuralCurveTrainerDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QString::fromUtf8(u8"神经网络轨迹曲线训练"));
    resize(740, 780);
    auto* root = new QVBoxLayout(this);
    auto* hint = new QLabel(QString::fromUtf8(
        u8"用当前 Windows 桌面的鼠标，在金色起点按住左键拖到绿色目标。"
        u8"建议录制 100～200 条自然轨迹；左右镜像会对齐后学习典型弯曲，曲线只改变移动方向。"
        u8"本窗口最多保留最近 200 条录制，关闭窗口后原始样本不保留。"
        u8"也可以直接导入其他项目导出的 .xmcurve 真人轨迹文件，按同样的规则转换后训练本项目的曲线。"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel(QString::fromUtf8(u8"本次录制条数")));
    rounds_ = new QSpinBox;
    rounds_->setObjectName("neuralRecordingRounds");
    rounds_->setRange(5, 200);
    rounds_->setValue(100);
    controls->addWidget(rounds_);
    append_ = new QCheckBox(QString::fromUtf8(u8"追加到本窗口已有轨迹"));
    append_->setChecked(true);
    controls->addWidget(append_);
    recordButton_ = new QPushButton(QString::fromUtf8(u8"开始录制"));
    recordButton_->setObjectName("neuralStartRecording");
    controls->addWidget(recordButton_);
    trainButton_ = new QPushButton(QString::fromUtf8(u8"训练已有轨迹"));
    trainButton_->setObjectName("neuralTrainExisting");
    trainButton_->setEnabled(false);
    controls->addWidget(trainButton_);
    importButton_ = new QPushButton(QString::fromUtf8(u8"导入 .xmcurve 轨迹…"));
    importButton_->setObjectName("neuralImportXmCurve");
    importButton_->setToolTip(QString::fromUtf8(
        u8"读取其他项目导出的 .xmcurve 文件里的真人鼠标轨迹，转换后立即训练。\n"
        u8"文件只作为数据读取，不会运行任何代码；导入后仍需点击“应用到当前热键”才会生效。"));
    controls->addWidget(importButton_);
    controls->addStretch();
    root->addLayout(controls);

    canvas_ = new NeuralCurveTrainingCanvas;
    canvas_->setObjectName("neuralTrainingCanvas");
    root->addWidget(canvas_, 1);
    status_ = new QLabel(QString::fromUtf8(u8"等待录制"));
    root->addWidget(status_);

    preview_ = new CurveCanvas;
    preview_->setEnabled(false);
    preview_->hide();
    root->addWidget(preview_);
    quality_ = new QLabel;
    quality_->setWordWrap(true);
    quality_->hide();
    root->addWidget(quality_);

    auto* actions = new QHBoxLayout;
    exportButton_ = new QPushButton(QString::fromUtf8(u8"导出原始轨迹 CSV"));
    applyButton_ = new QPushButton(QString::fromUtf8(u8"应用到当前热键"));
    applyButton_->setObjectName("neuralApplyCurve");
    auto* closeButton = new QPushButton(QString::fromUtf8(u8"关闭"));
    exportButton_->setEnabled(false);
    applyButton_->setEnabled(false);
    actions->addWidget(exportButton_);
    randomButton_ = new QPushButton(QString::fromUtf8(u8"一键随机曲线"));
    randomButton_->setObjectName("neuralRandomCurve");
    actions->addWidget(randomButton_);
    connect(randomButton_, &QPushButton::clicked, this, [this] {
        if (training_ || canvas_->recording()) return;
        result_ = boss::randomNeuralCurve(QRandomGenerator::global()->generate());
        showResult();
        quality_->setText(QString::fromUtf8(u8"随机生成的平滑曲线，未使用录制样本，不提供拟合评分。"));
        quality_->show();
        status_->setText(QString::fromUtf8(u8"已生成新曲线，可再次随机，满意后点击「应用到当前热键」。"));
    });
    actions->addStretch();
    actions->addWidget(closeButton);
    actions->addWidget(applyButton_);
    root->addLayout(actions);

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(50);
    connect(pollTimer_, &QTimer::timeout, this, [this] { pollTraining(); });
    connect(recordButton_, &QPushButton::clicked, this, [this] {
        if (canvas_->recording())
        {
            canvas_->stopRecording();
            setRecordingUi(false);
            status_->setText(QString::fromUtf8(u8"录制已停止；可勾选追加后继续"));
            exportButton_->setEnabled(!canvas_->trajectories().empty());
        }
        else
        {
            result_ = {};
            importSummary_.clear();
            applyButton_->setEnabled(false);
            preview_->hide();
            quality_->hide();
            canvas_->startRecording(rounds_->value(), append_->isChecked());
            setRecordingUi(true);
        }
    });
    canvas_->progressChanged = [this](int accepted, int requested, int rejected) {
        status_->setText(QString::fromUtf8(u8"本次有效 %1 / %2，需重录 %3；总样本 %4")
                         .arg(accepted).arg(requested).arg(rejected)
                         .arg(canvas_->trajectories().size()));
        if (!canvas_->rejectionReason().isEmpty())
            status_->setText(status_->text() + QStringLiteral("\n") + canvas_->rejectionReason());
    };
    canvas_->collectionFinished = [this] { launchTraining(); };
    connect(trainButton_, &QPushButton::clicked, this, [this] { launchTraining(); });
    connect(importButton_, &QPushButton::clicked, this, [this] {
        if (importing_ || training_ || canvas_->recording()) return;
        const QString path = QFileDialog::getOpenFileName(
            this, QString::fromUtf8(u8"导入其他项目的 xmcurve 轨迹"), QString(),
            QString::fromUtf8(u8"xmcurve 轨迹 (*.xmcurve);;所有文件 (*)"));
        if (!path.isEmpty()) importXmCurve(path);
    });
    connect(exportButton_, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QString::fromUtf8(u8"导出训练轨迹"),
            QStringLiteral("curve_training.csv"), QStringLiteral("CSV (*.csv)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            status_->setText(QString::fromUtf8(u8"导出失败：文件无法写入"));
            return;
        }
        QTextStream stream(&file);
        stream << "trajectory,progress,deviation\n";
        const auto& trajectories = canvas_->trajectories();
        for (size_t i = 0; i < trajectories.size(); ++i)
            for (const auto& point : trajectories[i])
                stream << i << ',' << point.progress << ',' << point.deviation << '\n';
        status_->setText(QString::fromUtf8(u8"已导出 %1 条轨迹").arg(trajectories.size()));
    });
    connect(applyButton_, &QPushButton::clicked, this, [this] {
        if (!result_.success || !onApply) return;
        onApply(result_);
        accept();
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
}

void NeuralCurveTrainerDialog::setRecordingUi(bool recording)
{
    const bool idle = !recording && !training_ && !importing_;
    rounds_->setEnabled(!recording);
    append_->setEnabled(!recording);
    trainButton_->setEnabled(idle && canvas_->trajectories().size() >= 5);
    randomButton_->setEnabled(idle);
    importButton_->setEnabled(idle);
    recordButton_->setText(recording ? QString::fromUtf8(u8"停止录制")
                                     : QString::fromUtf8(u8"开始录制"));
}

void NeuralCurveTrainerDialog::launchTraining()
{
    if (training_ || canvas_->recording() || canvas_->trajectories().size() < 5) return;
    result_ = {};
    applyButton_->setEnabled(false);
    preview_->hide();
    quality_->hide();
    setRecordingUi(false);
    exportButton_->setEnabled(true);
    recordButton_->setEnabled(false);
    trainButton_->setEnabled(false);
    randomButton_->setEnabled(false);
    importButton_->setEnabled(false);
    status_->setText(QString::fromUtf8(u8"正在后台训练，并评估留出的轨迹…"));
    auto data = canvas_->trajectories();
    training_ = std::make_shared<TrainingState>();
    auto state = training_;
    try {
        std::thread([data = std::move(data), state]() mutable {
            boss::NeuralCurveTrainResult result;
            try { result = boss::trainNeuralCurve(data); }
            catch (const std::exception& e) { result.error = std::string("训练失败：") + e.what(); }
            catch (...) { result.error = "训练失败：未知错误，可重新训练"; }
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result = std::move(result);
            state->ready = true;
        }).detach();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->result.error = std::string("无法启动训练：") + e.what();
        state->ready = true;
    }
    pollTimer_->start();
}

void NeuralCurveTrainerDialog::importXmCurve(const QString& path)
{
    if (training_ || importing_ || canvas_->recording()) return;
    result_ = {};
    applyButton_->setEnabled(false);
    preview_->hide();
    quality_->hide();
    importing_ = std::make_shared<ImportState>();
    auto state = importing_;
    setRecordingUi(false);
    recordButton_->setEnabled(false);
    status_->setText(QString::fromUtf8(u8"正在读取并转换 xmcurve 轨迹…"));
    // Parsing can take a moment for a large file, so keep it off the UI thread.
    const auto work = [path, state] {
        std::vector<boss::NeuralTrajectory> trajectories;
        xm_curve_import::Report report;
        QString error;
        const bool ok = xm_curve_import::load(path, trajectories, report, error);
        QString summary;
        if (ok) {
            summary = QString::fromUtf8(u8"已导入 %1 / %2 条轨迹（还原画布 %3×%4 像素")
                          .arg(report.accepted).arg(report.samples)
                          .arg(report.canvasWidth).arg(report.canvasHeight);
            if (!report.profileName.isEmpty())
                summary += QString::fromUtf8(u8"，来自“%1”").arg(report.profileName);
            summary += QString::fromUtf8(u8"）");
            if (report.rejected > 0)
                summary += QString::fromUtf8(u8"，%1 条未通过校验已跳过").arg(report.rejected);
            if (report.unreadable > 0)
                summary += QString::fromUtf8(u8"，%1 条无法还原坐标已跳过").arg(report.unreadable);
            if (report.thinned > 0)
                summary += QString::fromUtf8(u8"，为控制训练耗时均匀抽样 %1 条").arg(report.thinned);
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->ok = ok;
        state->error = error;
        state->summary = summary;
        state->trajectories = std::move(trajectories);
        state->ready = true;
    };
    try {
        std::thread(work).detach();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->error = QString::fromUtf8(u8"无法启动导入：") + QString::fromUtf8(e.what());
        state->ready = true;
    }
    pollTimer_->start();
}

void NeuralCurveTrainerDialog::pollTraining()
{
    if (importing_) {
        bool ok = false;
        QString error, summary;
        std::vector<boss::NeuralTrajectory> trajectories;
        {
            std::lock_guard<std::mutex> lock(importing_->mutex);
            if (!importing_->ready) return;
            ok = importing_->ok;
            error = std::move(importing_->error);
            summary = std::move(importing_->summary);
            trajectories = std::move(importing_->trajectories);
        }
        importing_.reset();
        pollTimer_->stop();
        recordButton_->setEnabled(true);
        setRecordingUi(false);
        if (!ok) {
            status_->setText(QString::fromUtf8(u8"导入失败：") + error);
            return;
        }
        canvas_->addImportedTrajectories(std::move(trajectories));
        exportButton_->setEnabled(true);
        importSummary_ = summary;
        launchTraining();
        status_->setText(summary + QString::fromUtf8(u8"；正在后台训练，并评估留出的轨迹…"));
        return;
    }
    if (!training_) return;
    {
        std::lock_guard<std::mutex> lock(training_->mutex);
        if (!training_->ready) return;
        result_ = std::move(training_->result);
    }
    training_.reset();
    pollTimer_->stop();
    recordButton_->setEnabled(true);
    setRecordingUi(false);
    if (!result_.success)
    {
        status_->setText(QString::fromUtf8(result_.error.c_str()));
        return;
    }
    showResult();
    showQuality(result_);
    status_->setText((importSummary_.isEmpty() ? QString() : importSummary_ + QStringLiteral("\n")) +
                     QString::fromUtf8(u8"训练完成。满意后点击「应用到当前热键」。"));
}

void NeuralCurveTrainerDialog::showResult()
{
    std::vector<float> preview(512);
    for (size_t i = 0; i < preview.size(); ++i)
        preview[i] = static_cast<float>(boss::evaluateNeuralCurve(
            result_.weights, static_cast<double>(i) / (preview.size() - 1)));
    preview_->setSamples(preview);
    preview_->show();
    applyButton_->setEnabled(true);
}

void NeuralCurveTrainerDialog::showQuality(const boss::NeuralCurveTrainResult& result)
{
    const auto& q = result.quality;
    const double gap = q.validationRmse - q.trainingRmse;
    double peakDeviation = 0.0;
    for (int i = 0; i <= 256; ++i)
        peakDeviation = std::max(peakDeviation, std::abs(boss::evaluateNeuralCurve(
            result.weights, static_cast<double>(i) / 256.0)));
    const bool improvesOnStraight = q.baselineRmse > 0.001 &&
        q.validationRmse <= q.baselineRmse * 0.95;
    const char* grade = q.baselineRmse <= 0.001 ? u8"样本本身接近直线" :
        !improvesOnStraight ? u8"轨迹形状差异较大，建议检查样本" :
        (q.validationRmse < 0.08 && q.validationP95 < 0.18 && gap < 0.05)
        ? u8"良好" : (q.validationRmse < 0.15 && q.validationP95 < 0.30)
        ? u8"可试用" : u8"建议补录或重录";
    quality_->setText(QString::fromUtf8(
        u8"形状拟合质量（镜像对齐后）：%1｜训练 %2 条、独立验证 %3 条\n"
        u8"训练偏差 %4%，验证偏差 %5% 路径长度；95% 点偏差不超过 %6%（直线基线 %7%）。\n"
        u8"斜率变化量 %8（越低越平顺），最大弯曲 %9%，起点和终点固定归零。\n"
        u8"这只衡量录制路径的拟合一致性，不代表游戏命中率。")
        .arg(QString::fromUtf8(grade))
        .arg(q.trainingTrajectories).arg(q.validationTrajectories)
        .arg(q.trainingRmse * 100.0, 0, 'f', 1)
        .arg(q.validationRmse * 100.0, 0, 'f', 1)
        .arg(q.validationP95 * 100.0, 0, 'f', 1)
        .arg(q.baselineRmse * 100.0, 0, 'f', 1)
        .arg(q.slopeVariation, 0, 'f', 3)
        .arg(peakDeviation * 100.0, 0, 'f', 1));
    quality_->show();
}
