#include "widgets/NeuralCurveTrainer.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPushButton>

#include <chrono>
#include <thread>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    NeuralCurveTrainerDialog dialog;
    bool applied = false;
    dialog.onApply = [&](const boss::NeuralCurveTrainResult& result) {
        applied = result.success && result.quality.validationTrajectories >= 1;
    };
    dialog.show();
    app.processEvents();
    if (argc > 1) dialog.grab().save(argv[1]);
    auto* canvas = dynamic_cast<NeuralCurveTrainingCanvas*>(
        dialog.findChild<QWidget*>("neuralTrainingCanvas"));
    auto* start = dialog.findChild<QPushButton*>("neuralStartRecording");
    auto* apply = dialog.findChild<QPushButton*>("neuralApplyCurve");
    if (!canvas || !start || !apply || !dialog.isVisible()) return 1;

    start->click();
    for (int round = 0; round < 20; ++round)
    {
        const QPointF from = canvas->startPoint();
        const QPointF to = canvas->targetPoint();
        QMouseEvent press(QEvent::MouseButtonPress, from,
                          canvas->mapToGlobal(from.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &press);
        for (int step = 1; step <= 16; ++step)
        {
            const QPointF point = from + (to - from) * (static_cast<double>(step) / 16.0);
            QMouseEvent move(QEvent::MouseMove, point,
                             canvas->mapToGlobal(point.toPoint()),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(canvas, &move);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, to,
                            canvas->mapToGlobal(to.toPoint()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &release);
    }

    for (int i = 0; i < 500 && !apply->isEnabled(); ++i)
    {
        app.processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (canvas->trajectories().size() != 20 || !apply->isEnabled()) return 1;
    apply->click();
    return applied ? 0 : 1;
}
