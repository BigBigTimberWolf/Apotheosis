#pragma once

#include "mouse/neural_curve.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <cmath>

namespace neural_curve_file {

inline bool save(const QString& path, const boss::NeuralCurveTrainResult& model,
                 QString& error)
{
    if (!model.success) { error = QStringLiteral("当前热键没有可导出的神经曲线"); return false; }
    QJsonArray weights;
    for (float value : model.weights) {
        if (!std::isfinite(value) || std::abs(value) > 10000.0f) {
            error = QStringLiteral("神经曲线包含无效权重"); return false;
        }
        weights.append(static_cast<double>(value));
    }
    const auto validMetric = [](double value) {
        return std::isfinite(value) && value >= 0.0 && value <= 10000.0;
    };
    const int training = model.quality.trainingTrajectories;
    const int validation = model.quality.validationTrajectories;
    if (training < 0 || validation < 0 || training > 1000000 ||
        validation > 1000000 - training ||
        !validMetric(model.quality.validationRmse) ||
        !validMetric(model.quality.validationP95) ||
        !validMetric(model.quality.slopeVariation)) {
        error = QStringLiteral("神经曲线统计信息无效"); return false;
    }
    QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("apotheosis-neural-curve")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("weights"), weights},
        {QStringLiteral("examples"), training + validation},
        {QStringLiteral("validation_rmse"), model.quality.validationRmse},
        {QStringLiteral("validation_p95"), model.quality.validationP95},
        {QStringLiteral("slope_variation"), model.quality.slopeVariation}
    };
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        error = QStringLiteral("无法写入曲线文件：%1").arg(file.errorString());
        return false;
    }
    return true;
}

inline bool load(const QString& path, boss::NeuralCurveTrainResult& model,
                 QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("无法读取曲线文件：%1").arg(file.errorString());
        return false;
    }
    if (file.size() > 32768) { error = QStringLiteral("曲线文件过大"); return false; }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        error = QStringLiteral("曲线文件不是有效 JSON"); return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("apotheosis-neural-curve") ||
        root.value(QStringLiteral("version")).toInt() != 1) {
        error = QStringLiteral("曲线文件格式或版本不受支持"); return false;
    }
    const QJsonArray weights = root.value(QStringLiteral("weights")).toArray();
    if (weights.size() != 25) {
        error = QStringLiteral("曲线必须包含 25 个网络权重"); return false;
    }
    boss::NeuralCurveTrainResult loaded;
    for (int i = 0; i < 25; ++i) {
        if (!weights[i].isDouble()) {
            error = QStringLiteral("曲线权重含非数字值"); return false;
        }
        const double value = weights[i].toDouble();
        if (!std::isfinite(value) || std::abs(value) > 10000.0) {
            error = QStringLiteral("曲线权重超出有效范围"); return false;
        }
        loaded.weights[static_cast<size_t>(i)] = static_cast<float>(value);
    }
    auto metric = [&](const char* key, double& out) {
        const QJsonValue value = root.value(QString::fromLatin1(key));
        if (!value.isDouble()) return false;
        out = value.toDouble();
        return std::isfinite(out) && out >= 0.0 && out <= 10000.0;
    };
    const QJsonValue examples = root.value(QStringLiteral("examples"));
    const int count = examples.toInt(-1);
    if (!examples.isDouble() || count < 0 || count > 1000000 ||
        !metric("validation_rmse", loaded.quality.validationRmse) ||
        !metric("validation_p95", loaded.quality.validationP95) ||
        !metric("slope_variation", loaded.quality.slopeVariation)) {
        error = QStringLiteral("曲线统计信息无效"); return false;
    }
    loaded.quality.trainingTrajectories = count;
    loaded.success = true;
    model = loaded;
    return true;
}

} // namespace neural_curve_file
