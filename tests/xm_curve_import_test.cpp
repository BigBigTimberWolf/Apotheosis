#include "widgets/XmCurveImport.h"
#include "xm_curve_fixture.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace {
int failures = 0;
void check(bool okay, const char* message)
{
    if (!okay) { std::printf("FAIL: %s\n", message); ++failures; }
}

double peak(const boss::NeuralTrajectory& t)
{
    double best = 0.0;
    for (const auto& p : t) best = std::max(best, std::abs(p.deviation));
    return best;
}

struct Loaded
{
    bool ok = false;
    std::vector<boss::NeuralTrajectory> trajectories;
    xm_curve_import::Report report;
    QString error;
};

Loaded loadBytes(const QTemporaryDir& dir, const QByteArray& bytes, const char* name = "t.xmcurve")
{
    Loaded result;
    const QString path = dir.filePath(QString::fromLatin1(name));
    if (!xm_fixture::write(path, bytes)) { result.error = QStringLiteral("fixture write failed"); return result; }
    result.ok = xm_curve_import::load(path, result.trajectories, result.report, result.error);
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    using xm_fixture::Canvas;

    // 1. A normal file: the pixel grid is recovered and strokes keep their bend.
    {
        QJsonArray samples = xm_fixture::strokes(Canvas{}, 30, 0.08, 111.0);
        for (const QJsonValue& v : xm_fixture::strokes(Canvas{}, 10, 0.08, 222.0)) samples.append(v);
        const auto r = loadBytes(dir, xm_fixture::document(samples));
        check(r.ok, "a well formed file loads");
        check(r.report.samples == 40 && r.report.accepted == 40 && r.report.rejected == 0 &&
              r.report.unreadable == 0, "every sample is accepted");
        check(r.report.canvasWidth == 1043 && r.report.canvasHeight == 420,
              "the canvas pixel size is recovered from the data");
        check(r.report.profileName == QStringLiteral("测试曲线"), "profile name is reported");
        bool shapeOk = r.trajectories.size() == 40;
        bool bendOk = true;
        for (const auto& t : r.trajectories) {
            shapeOk &= t.size() == static_cast<size_t>(boss::kNeuralTrainingSamples) &&
                       t.front().progress == 0.0 && t.back().progress == 1.0 &&
                       t.front().deviation == 0.0 && t.back().deviation == 0.0;
            bendOk &= peak(t) > 0.06 && peak(t) < 0.10;
        }
        check(shapeOk, "trajectories use the trainer's 256-point, zero-at-the-ends form");
        check(bendOk, "the known 8% bend survives import (aspect ratio handled)");

        // The imported strokes train the local curve like recorded ones do.
        const auto trained = boss::trainNeuralCurve(r.trajectories);
        check(trained.success, "imported strokes train a curve");
        double best = 0.0;
        for (int i = 0; i <= 256; ++i)
            best = std::max(best, std::abs(boss::evaluateNeuralCurve(trained.weights, i / 256.0)));
        check(best > 0.06 && best < 0.10, "the trained curve reproduces the imported bend");
    }

    // 2. Sessions recorded on differently sized canvases are each recovered.
    {
        QJsonArray samples = xm_fixture::strokes(Canvas{1043, 420}, 24, 0.08, 1.0);
        for (const QJsonValue& v : xm_fixture::strokes(Canvas{800, 600}, 24, 0.08, 2.0)) samples.append(v);
        const auto r = loadBytes(dir, xm_fixture::document(samples));
        check(r.ok && r.report.accepted == 48 && r.report.unreadable == 0,
              "sessions with different canvas sizes are both accepted");
        bool bendOk = r.trajectories.size() == 48;
        for (const auto& t : r.trajectories) bendOk &= peak(t) > 0.06 && peak(t) < 0.10;
        check(bendOk, "each session is scaled with its own canvas size");
    }

    // 3. The same stroke as a live recording and as an import normalizes identically.
    {
        const QJsonObject sample = xm_fixture::sample(Canvas{}, 500, 200, 0.4, 150, 0.05, 9.0);
        QJsonArray samples{sample};
        for (const QJsonValue& v : xm_fixture::strokes(Canvas{}, 11, 0.05, 9.0)) samples.append(v);
        const auto r = loadBytes(dir, xm_fixture::document(samples));
        check(r.ok && !r.trajectories.empty(), "mixed file loads");
        std::vector<boss::NeuralPoint2D> stroke{{500.0, 200.0}};
        for (const QJsonValue& row : sample.value(QStringLiteral("points")).toArray()) {
            const QJsonArray a = row.toArray();
            stroke.push_back({a[3].toDouble() * 1043.0, a[4].toDouble() * 420.0});
        }
        std::string reason;
        // The file stores the target on a whole pixel, so compare against that pixel.
        const boss::NeuralPoint2D target{std::round(500.0 + std::cos(0.4) * 150.0),
                                         std::round(200.0 + std::sin(0.4) * 150.0)};
        const auto direct = boss::normalizeNeuralStroke(
            {500.0, 200.0}, target, 0.035 * 420.0, stroke, reason);
        bool same = !direct.empty() && direct.size() == r.trajectories.front().size();
        for (size_t i = 0; same && i < direct.size(); ++i)
            same = std::abs(direct[i].deviation - r.trajectories.front()[i].deviation) < 1e-3;
        check(same, "an imported stroke is normalized exactly like a recorded one");
    }

    // 4. Bad input is refused with a clear reason and never half-imported.
    {
        const QJsonArray ok = xm_fixture::strokes(Canvas{}, 12, 0.05, 5.0);
        auto r = loadBytes(dir, "this is not json");
        check(!r.ok && r.error.contains(QStringLiteral("JSON")) && r.trajectories.empty(),
              "non-JSON content is refused");
        r = loadBytes(dir, xm_fixture::document(ok, QStringLiteral("something_else")));
        check(!r.ok && r.error.contains(QStringLiteral("format")), "a different format is refused");
        r = loadBytes(dir, xm_fixture::document(ok, QStringLiteral("xm_custom_curve"), 5));
        check(!r.ok && r.error.contains(QStringLiteral("版本")), "an unverified version is refused");
        r = loadBytes(dir, xm_fixture::document(QJsonArray{}));
        check(!r.ok, "a file without samples is refused");
        r = loadBytes(dir, xm_fixture::document(ok).left(xm_fixture::document(ok).size() / 2));
        check(!r.ok && r.trajectories.empty(), "a truncated file is refused");
        QFile missing(dir.filePath(QStringLiteral("nope.xmcurve")));
        std::vector<boss::NeuralTrajectory> out;
        xm_curve_import::Report report;
        QString error;
        check(!xm_curve_import::load(missing.fileName(), out, report, error) && !error.isEmpty(),
              "a missing file is reported");
    }

    // 5. Malformed or hostile samples are rejected individually.
    {
        QJsonArray samples = xm_fixture::strokes(Canvas{}, 12, 0.05, 5.0);
        samples.append(QJsonObject{{QStringLiteral("points"), 7}});            // wrong shape
        QJsonObject huge = xm_fixture::sample(Canvas{}, 500, 200, 1.0, 150, 0.05, 5.0);
        huge[QStringLiteral("start")] = QJsonArray{1e300, 0.5};                  // absurd value
        samples.append(huge);
        QJsonObject tooShort = xm_fixture::sample(Canvas{}, 500, 200, 1.0, 20, 0.05, 5.0);
        samples.append(tooShort);                                                // start ~ target
        const auto r = loadBytes(dir, xm_fixture::document(samples));
        check(r.ok && r.report.accepted == 12 && r.report.rejected == 3,
              "bad samples are skipped and counted, good ones still import");
    }

    // 6. Data that is not on any pixel grid cannot be scaled, and says so.
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> unit(0.1, 0.9);
        QJsonArray samples;
        for (int i = 0; i < 12; ++i) {
            QJsonObject s = xm_fixture::sample(Canvas{}, 500, 200, 0.3 * i, 150, 0.05, 5.0);
            QJsonArray points;
            for (int k = 0; k < 40; ++k)
                points.append(QJsonArray{k + 1, 0, 0, unit(rng), unit(rng)});
            s[QStringLiteral("points")] = points;
            samples.append(s);
        }
        const auto r = loadBytes(dir, xm_fixture::document(samples));
        check(!r.ok && r.report.unreadable == 12 && r.error.contains(QStringLiteral("像素")),
              "coordinates off any pixel grid are refused with an explanation");
    }

    // 7. A huge file is thinned evenly rather than refused or made slow to train.
    {
        QJsonArray samples;
        for (int round = 0; round < 3100 / 31 + 1; ++round)
            for (const QJsonValue& v : xm_fixture::strokes(Canvas{}, 31, 0.05, 3.0)) samples.append(v);
        const auto r = loadBytes(dir, xm_fixture::document(samples));
        check(r.ok && r.trajectories.size() == static_cast<size_t>(xm_curve_import::kMaxTrajectories) &&
              r.report.accepted == static_cast<int>(samples.size()) &&
              r.report.thinned == r.report.accepted - xm_curve_import::kMaxTrajectories,
              "more strokes than the cap are thinned evenly and reported");
    }

    // 8. Routing helper used by the generic import dialog.
    {
        const QString byName = dir.filePath(QStringLiteral("a.XMCURVE"));
        check(xm_fixture::write(byName, "x") && xm_curve_import::isXmCurveFile(byName),
              "the .xmcurve extension is recognised");
        const QString byContent = dir.filePath(QStringLiteral("b.json"));
        check(xm_fixture::write(byContent, "{\"format\": \"xm_custom_curve\"}") &&
              xm_curve_import::isXmCurveFile(byContent), "the format tag is recognised in a renamed file");
        const QString other = dir.filePath(QStringLiteral("c.json"));
        check(xm_fixture::write(other, "{\"format\":\"apotheosis-neural-curve\"}") &&
              !xm_curve_import::isXmCurveFile(other), "this project's own curve files are not misrouted");
    }

    std::printf("xm curve import: %d failures\n", failures);
    return failures ? 1 : 0;
}
