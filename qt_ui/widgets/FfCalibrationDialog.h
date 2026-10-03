#pragma once

#include "control/recovered_pid.h"
#include "runtime/ff_calibration_session.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <array>
#include <functional>

// Header-only: the normal worker owns mouse sends; this dialog only arms,
// displays and explicitly applies an offline fit to a captured profile/bank.
class FfCalibrationDialog : public QDialog {
public:
    using Apply = std::function<bool(int, const control::FfCalibrationResult&)>;
    FfCalibrationDialog(int hotkey, QString name,
        std::array<control::RecoveredPidConfig, 3> banks, Apply apply, QWidget* parent = nullptr)
        : QDialog(parent), hotkey_(hotkey), banks_(banks), apply_(std::move(apply))
    {
        setWindowTitle(QStringLiteral("FF 自动标定 · %1").arg(name));
        setWindowModality(Qt::WindowModal); resize(560, 360);
        auto* layout = new QVBoxLayout(this);
        auto* hint = new QLabel(QStringLiteral(
            "在训练场对准一个静止目标，标定区域只保留一个检测框。\n"
            "选择要保存的参数档，并手动保持对应的开镜状态和倍率。\n"
            "点击开始后按住所选瞄准热键约 6 秒；程序会进行小幅往返拉枪。\n"
            "不要移动鼠标、走位或开火。松开热键、取消或关闭窗口会停止标定。\n"
            "标定期间暂停普通瞄准和自动扳机；正常跟踪不增加图像处理。"), this);
        hint->setWordWrap(true); layout->addWidget(hint);
        bank_ = new QComboBox(this); bank_->setObjectName("ffCalibrationBank");
        bank_->addItems({QStringLiteral("默认参数"), QStringLiteral("第二套参数"), QStringLiteral("开镜独立参数")});
        layout->addWidget(bank_);
        current_ = new QLabel(this); current_->setWordWrap(true); layout->addWidget(current_);
        confirmed_ = new QCheckBox(QStringLiteral("已保持目标、视角和倍率固定"), this);
        confirmed_->setObjectName("ffCalibrationStaticTarget"); layout->addWidget(confirmed_);
        status_ = new QLabel(QStringLiteral("标定结果确认后才会修改配置。"), this);
        status_->setObjectName("ffCalibrationStatus"); status_->setWordWrap(true); layout->addWidget(status_);
        start_ = new QPushButton(QStringLiteral("开始标定"), this); start_->setObjectName("ffCalibrationStart");
        start_->setEnabled(false); layout->addWidget(start_);
        applyButton_ = new QPushButton(QStringLiteral("应用到所选参数档"), this);
        applyButton_->setObjectName("ffCalibrationApply"); applyButton_->setEnabled(false); layout->addWidget(applyButton_);
        auto* cancel = new QPushButton(QStringLiteral("取消 / 关闭"), this); layout->addWidget(cancel);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(confirmed_, &QCheckBox::toggled, this, [this](bool checked) {
            start_->setEnabled(checked && !runtime::FfCalibrationSession::instance().active());
        });
        connect(bank_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            result_ = {}; applyButton_->setEnabled(false); refreshCurrent();
        });
        connect(start_, &QPushButton::clicked, this, [this] {
            auto& session = runtime::FfCalibrationSession::instance();
            if (!session.arm(hotkey_, bank_->currentIndex())) {
                status_->setText(QStringLiteral("已有标定在进行，请先结束。")); return;
            }
            result_ = {}; fitted_ = false; owned_ = true; fittedBank_ = bank_->currentIndex();
            bank_->setEnabled(false); confirmed_->setEnabled(false); start_->setEnabled(false);
            applyButton_->setEnabled(false); timer_->start(); poll();
        });
        connect(applyButton_, &QPushButton::clicked, this, [this] {
            if (!result_.valid || runtime::FfCalibrationSession::instance().active()) return;
            const auto s = runtime::FfCalibrationSession::instance().snapshot();
            if (s.state != runtime::FfCalibrationSession::State::Ready || s.hotkey != hotkey_ || s.bank != fittedBank_) {
                result_ = {}; applyButton_->setEnabled(false);
                status_->setText(QStringLiteral("标定已取消或设备已重置，请重新标定。")); return;
            }
            if (!apply_(fittedBank_, result_)) {
                status_->setText(QStringLiteral("热键或采集/设备配置已变化，结果未应用，请重新标定。")); return;
            }
            owned_ = false; accept();
        });
        timer_ = new QTimer(this); timer_->setInterval(100);
        connect(timer_, &QTimer::timeout, this, [this] { poll(); });
        connect(this, &QDialog::finished, this, [this](int) {
            timer_->stop(); if (owned_) runtime::FfCalibrationSession::instance().cancel(); owned_ = false;
        });
        refreshCurrent();
    }
    ~FfCalibrationDialog() override {
        if (owned_) runtime::FfCalibrationSession::instance().cancel();
    }
private:
    void refreshCurrent() {
        const auto& p = banks_[size_t(bank_->currentIndex())];
        current_->setText(QStringLiteral("当前换算：X %1、Y %2 px/count；延迟：%3")
            .arg(p.motionPixelsPerCountX, 0, 'f', 4).arg(p.motionPixelsPerCountY, 0, 'f', 4)
            .arg(p.motionDelayMs < 0 ? QStringLiteral("旧版估计窗口") : QStringLiteral("%1 ms").arg(p.motionDelayMs, 0, 'f', 1)));
    }
    void poll() {
        auto& session = runtime::FfCalibrationSession::instance();
        const auto s = session.snapshot();
        if (s.state == runtime::FfCalibrationSession::State::Ready) {
            if (!fitted_) {
                const auto samples = session.snapshot(true);
                result_ = control::fitFfCalibration(samples.observations, samples.moves); fitted_ = true;
            }
            status_->setText(result_.valid ? QStringLiteral(
                "X %1、Y %2 px/count；响应延迟约 %3 ms\n拟合误差：X %4、Y %5 px。松开热键后可应用。")
                .arg(result_.pixelsPerCount.x, 0, 'f', 4).arg(result_.pixelsPerCount.y, 0, 'f', 4)
                .arg(result_.delayMs, 0, 'f', 1).arg(result_.rmse.x, 0, 'f', 2).arg(result_.rmse.y, 0, 'f', 2)
                : QString::fromUtf8(result_.reason));
            applyButton_->setEnabled(result_.valid && !session.active());
        } else status_->setText(QStringLiteral("%1\n已完成 %2 / 16 次移动")
            .arg(QString::fromUtf8(s.message)).arg(s.completed));
        if (!session.active()) {
            timer_->stop(); bank_->setEnabled(true); confirmed_->setEnabled(true);
            start_->setEnabled(confirmed_->isChecked());
        }
    }
    int hotkey_ = -1, fittedBank_ = 0;
    bool fitted_ = false, owned_ = false;
    std::array<control::RecoveredPidConfig, 3> banks_;
    Apply apply_;
    control::FfCalibrationResult result_;
    QComboBox* bank_ = nullptr;
    QCheckBox* confirmed_ = nullptr;
    QLabel *current_ = nullptr, *status_ = nullptr;
    QPushButton *start_ = nullptr, *applyButton_ = nullptr;
    QTimer* timer_ = nullptr;
};
