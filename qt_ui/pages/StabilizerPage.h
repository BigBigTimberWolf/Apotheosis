#pragma once

#include <QWidget>
#include <vector>

class QDoubleSpinBox;
class CardWidget;

// ── 稳定器页 (全局选靶 + 目标稳定器) ────────────────────────────────────────
//
// ★ 为什么单独一页: 这 7 个参数直接决定【锁谁、锁得住锁不住】—— 选靶滞回决定
//   会不会在两个目标之间来回跳, 稳定器 4 项决定"这一帧的框还算不算同一个目标",
//   而每一次 Snap 都会把滤波器与 PID 硬复位。它们和「准星找色」是同一层的东西
//   (都作用于控制器【之前】的输入), 所以放在同一级。
//
// ★ 全部是【全局】参数: 所有热键、所有方案共用一套 (与每个热键各自的瞄准控制器
//   参数不同)。改完立即生效 —— 走 ConfigManager → ConfigBridge::syncToRuntime。
//
// ★ 历史: 这一段原来挂在「目标」页的一张卡里, 且标签颜色写死成 #D4D4D8 ——
//   在浅色主题下几乎看不见。搬到这里时改成全站统一的 FormKit 行样式。
class StabilizerPage : public QWidget
{
    Q_OBJECT

public:
    explicit StabilizerPage(QWidget* parent = nullptr);

private:
    void buildCard();
    void reloadFromRuntime();

    std::vector<QDoubleSpinBox*> m_spins;
    CardWidget* m_card = nullptr;
};
