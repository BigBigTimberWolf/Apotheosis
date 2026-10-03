#pragma once

// Import of ".xmcurve" files exported by another project's curve trainer.
//
// The format was reverse-engineered from a real export (no source of the other
// project exists here), so only what was observed is relied on:
//
//   { "format": "xm_custom_curve", "version": 4,
//     "profile": { "name": ... },
//     "samples": [ {
//         "session_id": <id>, "start": [x, y], "target": [x, y, radius],
//         "points": [ [t_us, dx_counts, dy_counts, x, y], ... ],
//         ... bins / timing metadata that is not needed here ... } ],
//     "training_evidence": { ... } }
//
// x/y are the on-screen pointer position normalized by the pixel size of the
// canvas the sample was recorded on; `radius` is relative to the shorter side.
// The file does not store that pixel size, but every coordinate is a whole
// number of pixels, so it is recovered from the data (per recording session).
// Each sample is then a recorded stroke exactly like one captured by this
// project's own training canvas, and goes through the same normalization
// (boss::normalizeNeuralStroke) before the local trainer fits the local curve.
//
// The file is untrusted input: everything is validated and bounded.

#include "mouse/neural_curve.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace xm_curve_import {

inline constexpr qint64 kMaxFileBytes = 256LL * 1024 * 1024;
inline constexpr int kSupportedVersion = 4;
inline constexpr int kMaxSamples = 50000;
inline constexpr int kMaxPointsPerSample = 20000;
// Training cost grows with the square of the trajectory count, so a very large
// file is thinned evenly instead of being refused.
inline constexpr int kMaxTrajectories = 3000;
inline constexpr double kDefaultTargetRadiusPx = 15.0;

struct Report
{
    QString profileName;
    int samples = 0;          // samples found in the file
    int accepted = 0;         // turned into training trajectories
    int rejected = 0;         // failed the same checks a live recording must pass
    int unreadable = 0;       // dropped because their pixel grid could not be recovered
    int thinned = 0;          // accepted but left out to bound the training cost
    int canvasWidth = 0;      // pixel grid of the first session, for the status text
    int canvasHeight = 0;
};

// Cheap check used to route a file chosen in a generic "import curve" dialog.
inline bool isXmCurveFile(const QString& path)
{
    if (QFileInfo(path).suffix().compare(QStringLiteral("xmcurve"), Qt::CaseInsensitive) == 0)
        return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    return file.read(512).contains("\"xm_custom_curve\"");
}

// Coordinates were normalized by an integer pixel size, so on the true size every
// value times that size is (up to float rounding) a whole number. Returns the
// smallest such size; false when the data is not on any pixel grid.
inline bool inferPixelGrid(const std::vector<double>& values, int& grid)
{
    if (values.size() < 200) return false;
    const size_t stride = std::max<size_t>(1, values.size() / 4000);
    std::vector<double> sample;
    for (size_t i = 0; i < values.size(); i += stride) sample.push_back(values[i]);
    const auto [lo, hi] = std::minmax_element(sample.begin(), sample.end());
    if (*hi - *lo < 0.05) return false; // no spread: the grid is not identifiable
    for (int n = 32; n <= 8192; ++n) {
        double sum = 0.0;
        for (const double v : sample) {
            const double px = v * n;
            sum += std::abs(px - std::round(px));
        }
        if (sum / static_cast<double>(sample.size()) < 0.02) { grid = n; return true; }
    }
    return false;
}

namespace detail {

struct RawSample
{
    QString session;
    boss::NeuralPoint2D start, target;
    double radius = 0.0; // relative to the shorter canvas side; 0 = not given
    std::vector<boss::NeuralPoint2D> points;
};

inline bool number(const QJsonValue& value, double& out)
{
    if (!value.isDouble()) return false;
    out = value.toDouble();
    return std::isfinite(out) && std::abs(out) <= 1.0e6;
}

inline bool readSample(const QJsonValue& value, RawSample& out)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QJsonArray start = object.value(QStringLiteral("start")).toArray();
    const QJsonArray target = object.value(QStringLiteral("target")).toArray();
    const QJsonArray points = object.value(QStringLiteral("points")).toArray();
    if (start.size() < 2 || target.size() < 2 || points.size() < 3 ||
        points.size() > kMaxPointsPerSample)
        return false;
    if (!number(start[0], out.start.x) || !number(start[1], out.start.y) ||
        !number(target[0], out.target.x) || !number(target[1], out.target.y))
        return false;
    if (target.size() >= 3 && !number(target[2], out.radius)) return false;
    out.points.reserve(static_cast<size_t>(points.size()));
    for (const QJsonValue& entry : points) {
        const QJsonArray row = entry.toArray();
        boss::NeuralPoint2D point;
        if (row.size() < 5 || !number(row[3], point.x) || !number(row[4], point.y))
            return false;
        out.points.push_back(point);
    }
    // Large ids exceed what a double holds exactly, which is fine for grouping.
    const QJsonValue session = object.value(QStringLiteral("session_id"));
    out.session = session.isDouble() ? QString::number(session.toDouble(), 'g', 17)
                                     : QStringLiteral("-");
    return true;
}

} // namespace detail

inline bool load(const QString& path, std::vector<boss::NeuralTrajectory>& out,
                 Report& report, QString& error)
{
    out.clear();
    report = {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("无法读取文件：%1").arg(file.errorString());
        return false;
    }
    if (file.size() > kMaxFileBytes) { error = QStringLiteral("文件过大（超过 256 MB）"); return false; }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        error = QStringLiteral("文件不是有效的 JSON，可能已损坏或被截断");
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("xm_custom_curve")) {
        error = QStringLiteral("这不是 xmcurve 轨迹文件（format 不匹配）");
        return false;
    }
    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version != kSupportedVersion) {
        error = QStringLiteral("暂不支持 xmcurve 版本 %1（目前只验证过版本 %2 的数据布局）")
                    .arg(version).arg(kSupportedVersion);
        return false;
    }
    const QJsonArray samples = root.value(QStringLiteral("samples")).toArray();
    if (samples.isEmpty()) { error = QStringLiteral("文件里没有轨迹样本"); return false; }
    if (samples.size() > kMaxSamples) { error = QStringLiteral("样本数量异常（超过 5 万条）"); return false; }
    report.profileName = root.value(QStringLiteral("profile")).toObject()
                             .value(QStringLiteral("name")).toString().left(64);
    report.samples = samples.size();

    // Read and validate every sample, grouped by recording session because each
    // session may have been recorded on a differently sized canvas.
    std::map<QString, std::vector<detail::RawSample>> sessions;
    for (const QJsonValue& value : samples) {
        detail::RawSample sample;
        if (detail::readSample(value, sample)) sessions[sample.session].push_back(std::move(sample));
        else ++report.rejected;
    }

    std::vector<boss::NeuralTrajectory> accepted;
    for (auto& entry : sessions) {
        auto& group = entry.second;
        std::vector<double> xs, ys;
        for (const auto& sample : group) {
            for (const auto& point : sample.points) { xs.push_back(point.x); ys.push_back(point.y); }
            xs.push_back(sample.start.x);  ys.push_back(sample.start.y);
            xs.push_back(sample.target.x); ys.push_back(sample.target.y);
        }
        int width = 0, height = 0;
        if (!inferPixelGrid(xs, width) || !inferPixelGrid(ys, height)) {
            report.unreadable += static_cast<int>(group.size());
            continue;
        }
        if (report.canvasWidth == 0) { report.canvasWidth = width; report.canvasHeight = height; }
        for (const auto& sample : group) {
            const auto toPixels = [&](boss::NeuralPoint2D p) {
                return boss::NeuralPoint2D{p.x * width, p.y * height};
            };
            const boss::NeuralPoint2D start = toPixels(sample.start);
            // A stroke begins at the start circle's centre, as on the local canvas,
            // then follows every recorded pointer position.
            std::vector<boss::NeuralPoint2D> stroke{start};
            for (const auto& point : sample.points) stroke.push_back(toPixels(point));
            const double radius = sample.radius > 0.0
                ? std::clamp(sample.radius * std::min(width, height), 4.0, 80.0)
                : kDefaultTargetRadiusPx;
            std::string reason;
            auto trajectory = boss::normalizeNeuralStroke(
                start, toPixels(sample.target), radius, stroke, reason);
            if (trajectory.empty()) ++report.rejected;
            else accepted.push_back(std::move(trajectory));
        }
    }
    if (accepted.empty()) {
        error = report.unreadable > 0
            ? QStringLiteral("无法从文件中还原屏幕像素尺寸，没有可用的轨迹")
            : QStringLiteral("文件里没有通过校验的轨迹（起点到目标太近、绕行过多或没有到达目标）");
        return false;
    }
    report.accepted = static_cast<int>(accepted.size());
    if (accepted.size() > static_cast<size_t>(kMaxTrajectories)) {
        for (int i = 0; i < kMaxTrajectories; ++i)
            out.push_back(std::move(accepted[static_cast<size_t>(i) * accepted.size() / kMaxTrajectories]));
        report.thinned = report.accepted - kMaxTrajectories;
    } else {
        out = std::move(accepted);
    }
    return true;
}

} // namespace xm_curve_import
