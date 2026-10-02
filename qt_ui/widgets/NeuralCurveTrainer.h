#pragma once

#include <QDialog>
#include <QPointF>
#include <QWidget>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "mouse/neural_curve.h"

class CurveCanvas;
class QLabel;
class QPushButton;
class QSpinBox;
class QCheckBox;
class QTimer;

class NeuralCurveTrainingCanvas final : public QWidget
{
public:
    explicit NeuralCurveTrainingCanvas(QWidget* parent = nullptr);

    void startRecording(int rounds, bool append);
    void stopRecording();
    bool recording() const { return recording_; }
    const std::vector<boss::NeuralTrajectory>& trajectories() const { return trajectories_; }
    QPointF startPoint() const { return start_; }
    QPointF targetPoint() const { return target_; }
    const QString& rejectionReason() const { return rejectionReason_; }

    std::function<void(int accepted, int requested, int rejected)> progressChanged;
    std::function<void()> collectionFinished;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void beginRound();
    void finishRound();
    void appendStroke(QPointF point);
    boss::NeuralTrajectory normalizeStroke(QString& reason) const;

    bool recording_ = false;
    bool dragging_ = false;
    int requested_ = 0;
    int acceptedThisRun_ = 0;
    int rejected_ = 0;
    QPointF start_;
    QPointF target_;
    std::vector<QPointF> stroke_;
    std::vector<boss::NeuralTrajectory> trajectories_;
    QString rejectionReason_;
};

class NeuralCurveTrainerDialog final : public QDialog
{
public:
    explicit NeuralCurveTrainerDialog(QWidget* parent = nullptr);

    std::function<void(const boss::NeuralCurveTrainResult&)> onApply;

private:
    struct TrainingState
    {
        std::mutex mutex;
        bool ready = false;
        boss::NeuralCurveTrainResult result;
    };

    void launchTraining();
    void pollTraining();
    void showResult();
    void showQuality(const boss::NeuralCurveTrainResult& result);
    void setRecordingUi(bool recording);

    NeuralCurveTrainingCanvas* canvas_ = nullptr;
    CurveCanvas* preview_ = nullptr;
    QSpinBox* rounds_ = nullptr;
    QCheckBox* append_ = nullptr;
    QPushButton* recordButton_ = nullptr;
    QPushButton* trainButton_ = nullptr;
    QPushButton* randomButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* quality_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    std::shared_ptr<TrainingState> training_;
    boss::NeuralCurveTrainResult result_;
};
