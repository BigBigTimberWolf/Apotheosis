#pragma once

#include <QWidget>
#include <vector>

// 手绘轨迹曲线画布（自由绘制）。
//
// 坐标约定（与 aim_path.h 的 Custom 模式一致）:
//   X 轴 = 行程进度 t ∈ [0, 1]      —— 从起点走到目标的百分比
//   Y 轴 = 横向偏移   ∈ [-1, 1]     —— 相对【弦长】的偏离比例
//   曲线从 (0, 0) 到 (1, 0)；Y 向上为正。
//
// ★ 采样点固定 kSamples 个（与 AimPathDriver::kCustomSamples 对齐），
//   X 均匀分布，只存 Y。绘制时按鼠标拖过的 x 位置写对应下标的 Y。
// ★ 画布只负责【编辑与显示】，不碰 config —— 由 AimSettingsPage 决定何时落盘。
class CurveCanvas : public QWidget
{
    Q_OBJECT

public:
    static constexpr int kSamples = 32768;

    explicit CurveCanvas(QWidget* parent = nullptr);

    // 取/设采样点。setSamples 会按 X 线性重采样成 kSamples 个点。
    std::vector<float> samples() const { return m_samples; }
    void setSamples(const std::vector<float>& s);

    // 清空成一条直线（全 0）。
    void clearCurve();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void curveChanged();          // 每笔结束时发一次（拖拽过程不发，免得频繁落盘）
    void curveEdited();           // 拖拽过程中发（供实时预览用）

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    QRect plotRect() const;                       // 曲线绘制区（留出边距）
    void writeAt(int px, int py);                 // 把像素坐标写进采样点
    void drawGrid(QPainter& p, const QRect& r);

    std::vector<float> m_samples;                 // size 恒为 kSamples
    bool m_dragging = false;
    int  m_hoverX = -1;                           // 悬停竖线（-1 = 不画）
    int  m_prevIdx = -1;                          // 上一笔落点下标（拖拽插值用）
};
