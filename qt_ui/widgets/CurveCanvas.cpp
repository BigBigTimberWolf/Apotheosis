#include "CurveCanvas.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {
// 鼠标拖拽时，相邻两次事件之间要插值补齐 —— 否则快速拖动会留下锯齿/断点。
// 用线性插值把 prev 到 cur 之间的采样点填满。
} // namespace

CurveCanvas::CurveCanvas(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_samples.assign(kSamples, 0.0f);
}

QSize CurveCanvas::sizeHint() const { return QSize(420, 160); }
QSize CurveCanvas::minimumSizeHint() const { return QSize(240, 110); }

void CurveCanvas::setSamples(const std::vector<float>& s)
{
    if (s.empty())
    {
        m_samples.assign(kSamples, 0.0f);
        update();
        return;
    }
    // 按 X 线性重采样到 kSamples 个点（文件里的点数可能不是 kSamples）。
    m_samples.resize(kSamples);
    const int n = static_cast<int>(s.size());
    for (int i = 0; i < kSamples; ++i)
    {
        const double pos = (kSamples > 1)
            ? static_cast<double>(i) * (n - 1) / (kSamples - 1) : 0.0;
        int i0 = static_cast<int>(std::floor(pos));
        int i1 = i0 + 1;
        if (i0 < 0) { i0 = 0; i1 = 0; }
        if (i1 > n - 1) { i1 = n - 1; }
        const double f = pos - i0;
        const double y0 = static_cast<double>(s[i0]);
        const double y1 = static_cast<double>(s[i1]);
        m_samples[i] = static_cast<float>(std::clamp(y0 + (y1 - y0) * f, -1.0, 1.0));
    }
    update();
}

void CurveCanvas::clearCurve()
{
    m_samples.assign(kSamples, 0.0f);
    update();
    emit curveChanged();
}

QRect CurveCanvas::plotRect() const
{
    // 左边留 0 位置（不标数字），四边留一点内边距。
    return rect().adjusted(10, 10, -10, -10);
}

void CurveCanvas::writeAt(int px, int py)
{
    const QRect r = plotRect();
    if (r.width() <= 0 || r.height() <= 0) return;

    // 像素 → 采样点下标（X）与值（Y）
    const double tx = std::clamp(
        static_cast<double>(px - r.left()) / std::max(1, r.width()), 0.0, 1.0);
    // Y 向上为正：屏幕 y 向下增长，所以要翻过来
    const double ty = std::clamp(
        1.0 - 2.0 * static_cast<double>(py - r.top()) / std::max(1, r.height()),
        -1.0, 1.0);

    const int idx = std::clamp(
        static_cast<int>(std::lround(tx * (kSamples - 1))), 0, kSamples - 1);

    // ★ 只在【起点到终点】之间插值 —— 但拖拽时相邻两次事件的 idx 可能差很多，
    //   中间不补的话会出现"跳变"，曲线看起来是断的。
    if (m_prevIdx >= 0 && std::abs(idx - m_prevIdx) > 1)
    {
        const int a = m_prevIdx, b = idx;
        const double ya = m_samples[a];
        const int step = (b > a) ? 1 : -1;
        const int dist = std::abs(b - a);
        for (int k = 1; k < dist; ++k)
        {
            const int ii = a + step * k;
            if (ii < 0 || ii >= kSamples) continue;
            m_samples[ii] = static_cast<float>(ya + (ty - ya) * k / dist);
        }
    }
    m_samples[idx] = static_cast<float>(ty);
    m_prevIdx = idx;
}

void CurveCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect r = plotRect();

    // 背景
    p.fillRect(r, QColor(0xFA, 0xFA, 0xFB));
    p.setPen(QPen(QColor(0, 0, 0, 20), 1));
    p.drawRect(r);

    drawGrid(p, r);

    // 曲线
    QPainterPath path;
    for (int i = 0; i < kSamples; ++i)
    {
        const double x = r.left() + static_cast<double>(i) / (kSamples - 1) * r.width();
        const double y = r.top() + (1.0 - (m_samples[i] + 1.0) / 2.0) * r.height();
        if (i == 0) path.moveTo(x, y);
        else        path.lineTo(x, y);
    }
    p.setPen(QPen(QColor(0x5E, 0x6A, 0xD2), 2.0));
    p.drawPath(path);

    // 起点/终点圆点：提示"两端必须落在中轴线上"
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x5E, 0x6A, 0xD2));
    const double yMid = r.top() + r.height() / 2.0;
    p.drawEllipse(QPointF(r.left(),  yMid), 3.0, 3.0);
    p.drawEllipse(QPointF(r.right(), yMid), 3.0, 3.0);
    p.setBrush(Qt::NoBrush);

    // 悬停竖线
    if (m_hoverX >= r.left() && m_hoverX <= r.right())
    {
        p.setPen(QPen(QColor(0x9C, 0xA3, 0xAF, 140), 1, Qt::DashLine));
        p.drawLine(m_hoverX, r.top(), m_hoverX, r.bottom());
    }
}

void CurveCanvas::drawGrid(QPainter& p, const QRect& r)
{
    // 中轴线（Y=0）实线，其余虚线
    const double yMid = r.top() + r.height() / 2.0;
    p.setPen(QPen(QColor(0, 0, 0, 60), 1));
    p.drawLine(QPointF(r.left(), yMid), QPointF(r.right(), yMid));

    p.setPen(QPen(QColor(0, 0, 0, 25), 1, Qt::DashLine));
    // 横向：Y = ±0.5，以及边界 ±1
    for (double v : { -0.5, 0.5 })
    {
        const double y = r.top() + (1.0 - (v + 1.0) / 2.0) * r.height();
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    // 纵向：t = 0.25 / 0.5 / 0.75
    for (double t : { 0.25, 0.5, 0.75 })
    {
        const double x = r.left() + t * r.width();
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }

    // 角标：只标最必要的两个数，避免画布变表格
    p.setPen(QColor(0x9C, 0xA3, 0xAF));
    QFont f = p.font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    p.drawText(QRect(r.left() + 2, r.top() + 1, 60, 12),  Qt::AlignLeft,  QStringLiteral("+1"));
    p.drawText(QRect(r.left() + 2, r.bottom() - 13, 60, 12), Qt::AlignLeft, QStringLiteral("-1"));
    p.drawText(QRect(r.left(), r.bottom() + 1, r.width(), 12), Qt::AlignLeft,
               QStringLiteral("起点"));
    p.drawText(QRect(r.left(), r.bottom() + 1, r.width(), 12), Qt::AlignRight,
               QStringLiteral("终点"));
}

void CurveCanvas::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_prevIdx = -1;          // 新的一笔：不和上一笔的落点连插值，否则会拖出一条直线
    writeAt(e->pos().x(), e->pos().y());
    update();
    emit curveEdited();
}

void CurveCanvas::mouseMoveEvent(QMouseEvent* e)
{
    m_hoverX = e->pos().x();
    if (m_dragging)
    {
        writeAt(e->pos().x(), e->pos().y());
        emit curveEdited();
    }
    update();
}

void CurveCanvas::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = false;
    emit curveEdited();
    emit curveChanged();      // 一笔结束才通知落盘
}

void CurveCanvas::leaveEvent(QEvent*)
{
    m_hoverX = -1;
    update();
}
