#pragma once

// Builds synthetic ".xmcurve" files for tests. The layout mirrors what was observed
// in a real export: pointer positions are whole pixels divided by the canvas size
// (stored as float32), the first point is where the stroke started and the stroke
// ends as soon as it is inside the target circle.

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cmath>

namespace xm_fixture {

struct Canvas
{
    int width = 1043;
    int height = 420;
};

constexpr double kPi = 3.14159265358979323846;

// One stroke of `length` pixels leaving (sx, sy) at `angle` radians, bending to the
// left of its direction by `bend` (a fraction of the length) at its midpoint.
inline QJsonObject sample(const Canvas& canvas, double sx, double sy, double angle,
                          double length, double bend, double sessionId, int steps = 70)
{
    const double ax = std::cos(angle), ay = std::sin(angle);
    const double px = -ay, py = ax;
    const auto norm = [&](double x, double y) {
        return QJsonArray{static_cast<double>(static_cast<float>(x / canvas.width)),
                          static_cast<double>(static_cast<float>(y / canvas.height))};
    };
    QJsonArray points;
    qint64 time = 1;
    for (int i = 0; i <= steps; ++i) {
        const double t = 0.97 * i / steps; // arrives inside the target circle
        const double along = t * length;
        const double aside = bend * length * std::sin(kPi * t);
        const double x = std::round(sx + ax * along + px * aside);
        const double y = std::round(sy + ay * along + py * aside);
        time += 3000;
        const QJsonArray position = norm(x, y);
        points.append(QJsonArray{time, 1, 0, position[0], position[1]});
    }
    const double tx = std::round(sx + ax * length), ty = std::round(sy + ay * length);
    QJsonArray target = norm(tx, ty);
    target.append(0.035); // radius relative to the shorter canvas side
    return QJsonObject{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("start"), norm(std::round(sx), std::round(sy))},
        {QStringLiteral("target"), target},
        {QStringLiteral("points"), points},
        {QStringLiteral("direction_bin"), 0},
        {QStringLiteral("distance_bin"), 0},
    };
}

// `count` strokes in varied directions, all with the same bend.
inline QJsonArray strokes(const Canvas& canvas, int count, double bend, double sessionId,
                          double length = 150.0)
{
    QJsonArray out;
    for (int i = 0; i < count; ++i) {
        const double angle = 2.0 * kPi * i / count;
        // Keep the whole stroke inside the canvas, away from its edges.
        const double sx = canvas.width * 0.5 + 40.0 * std::cos(angle * 3.0);
        const double sy = canvas.height * 0.5 + 25.0 * std::sin(angle * 2.0);
        out.append(sample(canvas, sx, sy, angle, length, bend, sessionId));
    }
    return out;
}

inline QByteArray document(const QJsonArray& samples, const QString& format = QStringLiteral("xm_custom_curve"),
                           int version = 4)
{
    QJsonObject root{
        {QStringLiteral("format"), format},
        {QStringLiteral("version"), version},
        {QStringLiteral("profile"), QJsonObject{{QStringLiteral("name"), QStringLiteral("测试曲线")}}},
        {QStringLiteral("samples"), samples},
        {QStringLiteral("training_evidence"), QJsonObject{}},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

inline bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace xm_fixture
