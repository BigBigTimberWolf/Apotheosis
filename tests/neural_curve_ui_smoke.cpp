#include "widgets/NeuralCurveTrainer.h"
#include "widgets/CurveCanvas.h"
#include "widgets/NeuralCurveFile.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>

#include <chrono>
#include <cstdio>
#include <cmath>
#include <thread>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    {
        QTemporaryDir directory;
        if (!directory.isValid()) return 1;
        const QString path = directory.filePath(QStringLiteral("curve.ancurve"));
        auto exported = boss::randomNeuralCurve(2026);
        exported.quality.trainingTrajectories = 12;
        exported.quality.validationRmse = .04;
        QString error;
        if (!neural_curve_file::save(path, exported, error)) return 1;
        boss::NeuralCurveTrainResult imported;
        if (!neural_curve_file::load(path, imported, error) ||
            imported.weights != exported.weights ||
            imported.quality.trainingTrajectories != 12 ||
            std::abs(imported.quality.validationRmse - .04) > 1e-9)
            return 1;
        imported.weights[0] = 99;
        QSaveFile invalid(path);
        if (!invalid.open(QIODevice::WriteOnly) ||
            invalid.write("{\"format\":\"apotheosis-neural-curve\",\"version\":1,\"weights\":[]}") < 0 ||
            !invalid.commit()) return 1;
        if (neural_curve_file::load(path, imported, error) || imported.weights[0] != 99)
            return 1;
    }

    {
        NeuralCurveTrainerDialog randomDialog;
        auto* button = randomDialog.findChild<QPushButton*>("neuralRandomCurve");
        auto* applyRandom = randomDialog.findChild<QPushButton*>("neuralApplyCurve");
        auto* rounds = randomDialog.findChild<QSpinBox*>("neuralRecordingRounds");
        if (!button || !applyRandom || !rounds || rounds->maximum() != 200 || rounds->value() != 100)
            return 1;
        bool received = false;
        randomDialog.onApply = [&](const boss::NeuralCurveTrainResult& r) {
            received = r.success && r.quality.trainingTrajectories == 0 &&
                std::abs(boss::evaluateNeuralCurve(r.weights, .5)) > .06;
        };
        button->click();
        if (!applyRandom->isEnabled()) return 1;
        if (argc > 1) {
            randomDialog.show();
            app.processEvents();
            randomDialog.grab().save(QString::fromLocal8Bit(argv[1]) + ".random.png");
        }
        applyRandom->click();
        if (!received) return 1;
    }

    {
        CurveCanvas drawing;
        drawing.resize(400,160);
        int saves=0;
        QObject::connect(&drawing,&CurveCanvas::curveChanged,[&]{++saves;});
        auto send=[&](QEvent::Type type,QPointF point) {
            QMouseEvent event(type,point,drawing.mapToGlobal(point.toPoint()),
                type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                Qt::NoModifier);
            QCoreApplication::sendEvent(&drawing,&event);
        };
        send(QEvent::MouseButtonPress,{10,80});
        send(QEvent::MouseMove,{100,40});
        send(QEvent::MouseButtonRelease,{390,80});
        if(saves!=1 || drawing.samples()[CurveCanvas::kSamples*3/4]<.05f) {
            std::puts("FAIL: hand-drawn release endpoint was not saved");
            return 1;
        }
    }

    // Verify the new 200-stroke session completes without the old 80 limit.
    {
        NeuralCurveTrainingCanvas probe;
        probe.resize(640, 360);
        int completed = 0;
        probe.collectionFinished = [&] { ++completed; };
        probe.startRecording(200, false);
        for (int j = 0; j < 200; ++j) {
            const auto from = probe.startPoint(), to = probe.targetPoint();
            for (double fraction : {0.0, .3, .6, 1.0}) {
                const auto point = from + (to - from) * fraction;
                QMouseEvent event(fraction == 0 ? QEvent::MouseButtonPress : QEvent::MouseMove,
                    point, probe.mapToGlobal(point.toPoint()),
                    fraction == 0 ? Qt::LeftButton : Qt::NoButton,
                    Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&probe, &event);
            }
        }
        if (probe.recording() || completed != 1 || probe.trajectories().size() != 200) return 1;
    }
    // Real desktop events may arrive sparsely or skip over the target circle.
    auto recordSparse = [&](bool finishOnRelease) {
        NeuralCurveTrainingCanvas probe;
        probe.resize(640,360);
        probe.startRecording(5,false);
        const QPointF from=probe.startPoint(), to=probe.targetPoint();
        auto send=[&](QEvent::Type type, double fraction) {
            const QPointF at=from+(to-from)*fraction;
            QMouseEvent event(type,at,probe.mapToGlobal(at.toPoint()),
                type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                Qt::NoModifier);
            QCoreApplication::sendEvent(&probe,&event);
        };
        send(QEvent::MouseButtonPress,0);
        if(finishOnRelease) {
            for(int i=1;i<=6;++i) send(QEvent::MouseMove,i*.12);
            send(QEvent::MouseButtonRelease,1);
        } else {
            send(QEvent::MouseMove,.3);
            send(QEvent::MouseMove,.6);
            send(QEvent::MouseMove,1.25);
        }
        return probe.trajectories().size()==1;
    };
    if(!recordSparse(true) || !recordSparse(false)) {
        std::puts("FAIL: sparse strokes or arrival on mouse release are rejected");
        return 1;
    }
    NeuralCurveTrainerDialog dialog;
    bool applied = false;
    dialog.onApply = [&](const boss::NeuralCurveTrainResult& result) {
        applied = result.success && result.quality.validationTrajectories >= 1 &&
            boss::evaluateNeuralCurve(result.weights, .5) > .05;
    };
    dialog.show();
    app.processEvents();
    if (argc > 1) dialog.grab().save(argv[1]);
    auto* canvas = dynamic_cast<NeuralCurveTrainingCanvas*>(
        dialog.findChild<QWidget*>("neuralTrainingCanvas"));
    auto* start = dialog.findChild<QPushButton*>("neuralStartRecording");
    auto* apply = dialog.findChild<QPushButton*>("neuralApplyCurve");
    auto* train = dialog.findChild<QPushButton*>("neuralTrainExisting");
    if (!canvas || !start || !apply || !train || !dialog.isVisible()) return 1;

    start->click();
    for (int round = 0; round < 5; ++round)
    {
        const QPointF from = canvas->startPoint();
        const QPointF to = canvas->targetPoint();
        QMouseEvent press(QEvent::MouseButtonPress, from,
                          canvas->mapToGlobal(from.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &press);
        for (int step = 1; step <= 16; ++step)
        {
            const double t=static_cast<double>(step)/16.0;
            const QPointF delta=to-from;
            const QPointF point = from + delta*t +
                QPointF(-delta.y(),delta.x())*(.18*std::sin(3.141592653589793*t));
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

    // A user need not finish the default twenty rounds to train five valid ones.
    start->click();
    if (!train->isEnabled()) return 1;
    train->click();

    for (int i = 0; i < 500 && !apply->isEnabled(); ++i)
    {
        app.processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (canvas->trajectories().size() != 5 || !apply->isEnabled()) return 1;
    if (argc > 1) dialog.grab().save(QString::fromLocal8Bit(argv[1]) + ".trained.png");
    apply->click();
    return applied ? 0 : 1;
}
