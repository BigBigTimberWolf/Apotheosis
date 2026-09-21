#include "pages/StabilizerPage.h"

#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QDoubleSpinBox>
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <functional>
#include <iterator>   // std::size

namespace
{

// 一行的说明。★ 说明文字与原「目标」页那张卡逐字一致 —— 只是换了位置与外观,
// 不改语义, 免得老用户按旧说明调参时对不上。
struct StabItem
{
    const char* label;
    double lo, hi, step, def;
    const char* tip;
    std::function<double()> getter;
    std::function<void(double)> setter;
};

QLabel* makeHint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setProperty("class", "hint");
    return l;
}

void attachTip(QWidget* row, const QString& tip)
{
    if (!row || tip.isEmpty()) return;
    row->setToolTip(tip);
    for (QWidget* w : row->findChildren<QWidget*>())
        w->setToolTip(tip);
}

}

StabilizerPage::StabilizerPage(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    scroll->setWidget(content);

    buildCard();
    layout->addWidget(m_card);
    layout->addStretch();

    reloadFromRuntime();
    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, &StabilizerPage::reloadFromRuntime);
}

void StabilizerPage::buildCard()
{
    m_card = new CardWidget(QString::fromUtf8(u8"全局选靶与目标稳定器"),
                            QStringLiteral("layers-intersect"));
    auto* cl = m_card->contentLayout();

    cl->addWidget(makeHint(QString::fromUtf8(
        u8"★ 这里是【全局视觉感知层】：对所有热键、所有配置方案统一生效 —— "
        u8"不是每个热键各一套（那是「瞄准设置」里的瞄准控制器参数）。\n"
        u8"★ 它作用于控制器的【输入】：先按这些规则挑出「本拍该瞄哪个框、这个框还算不算"
        u8"同一个目标」，之后的滤波与 PID 才接手。\n"
        u8"★ 判定为换目标 / 瞬移（Snap）时，滤波器与 PID 会被【硬复位】—— 所以这几项"
        u8"直接决定「锁得住锁不住」，比它们看起来重要。\n"
        u8"★ 改完立即生效，不用重启会话。画面预览窗里会画出【稳定之后的框】，"
        u8"可以直接对着预览调，不用猜。")));

    auto& cm = ConfigManager::instance();

    const StabItem items[] = {
        { "选靶滞回倍数", 1.0, 10.0, 0.05, 1.3,
          "已锁定目标时，新候选必须比它『近这么多倍』才会换目标。\n"
          "★ 1.0 = 没有滞回（每帧都选最近的）。\n"
          "★ 不加滞回时，两个目标交替成为『最近』会让滤波器每帧复位 ——\n"
          "  滤波等于白做。所以它不是一个手感旋钮，是滤波能否生效的前提。",
          [&cm]() { return cm.targetHysteresisRatio(); },
          [&cm](double v) { cm.setTargetHysteresisRatio(v); }
        },
        { "选靶距离上限 (0=不限)", 0.0, 5000.0, 5.0, 0.0,
          "距准星超过这个距离（检测像素）的候选不参与选靶。\n"
          "★ 0 = 不限制。默认 0，因为距离门控目前由 FOV 椭圆承担，\n"
          "  这里再设一道是重复的。",
          [&cm]() { return cm.targetMaxDistancePx(); },
          [&cm](double v) { cm.setTargetMaxDistancePx(v); }
        },
        { "稳定器·认目标中心系数", 0.001, 10.0, 0.05, 0.5,
          "『这一帧的框和上一帧是同一个目标吗』的判据：\n"
          "中心距离 < 上一帧框对角线 × 该系数 就算同一个目标。\n"
          "调大 = 更容易认成同一个（目标跳一下也接着跟）；\n"
          "调小 = 更容易判成换目标（会触发滤波复位）。",
          [&cm]() { return cm.targetMatchCenterRatio(); },
          [&cm](double v) { cm.setTargetMatchCenterRatio(v); }
        },
        { "稳定器·面积容差倍数", 1.0, 100.0, 0.1, 2.0,
          "面积比超出 [1/该值, 该值] 就判为换目标。\n"
          "目标跑远/跑近时框面积本来就会变，这个容差就是留给它的。\n"
          "★ 不能小于 1（那是个自相矛盾的区间）。",
          [&cm]() { return cm.targetAreaRatioTol(); },
          [&cm](double v) { cm.setTargetAreaRatioTol(v); }
        },
        { "稳定器·突变系数 (Snap)", 0.001, 100.0, 0.05, 1.15,
          "本帧位移超过『上一帧框对角线 × 该系数』就判为瞬移（Snap）。\n"
          "瞬移会触发滤波与 PID 的硬重置。\n"
          "调小 = 更敏感（真的换目标时反应快，但抖动也可能误判）；\n"
          "调大 = 更宽容（可能把真换目标当成目标在快速移动）。",
          [&cm]() { return cm.targetKSnapMult(); },
          [&cm](double v) { cm.setTargetKSnapMult(v); }
        },
        { "稳定器·最小宽高比", 0.001, 100.0, 0.05, 0.2,
          "宽高比低于它就当作离谱误检丢掉（太细太长）。\n"
          "★ 会自动与最大值排序，保证 min ≤ max。",
          [&cm]() { return cm.targetMinAspect(); },
          [&cm](double v) { cm.setTargetMinAspect(v); }
        },
        { "稳定器·最大宽高比", 0.001, 100.0, 0.05, 5.0,
          "宽高比高于它就当作离谱误检丢掉（太扁太宽）。",
          [&cm]() { return cm.targetMaxAspect(); },
          [&cm](double v) { cm.setTargetMaxAspect(v); }
        },
    };

    m_spins.clear();
    m_spins.reserve(std::size(items));

    for (const auto& it : items)
    {
        auto* spin = new QDoubleSpinBox;
        spin->setRange(it.lo, it.hi);
        spin->setSingleStep(it.step);
        spin->setDecimals(3);
        spin->setValue(it.getter());

        // FormKit 行: 标签用主题的 secondary 色(浅色主题下可读), 控件右侧留白对齐。
        QWidget* row = FormKit::fieldRow(QString::fromUtf8(it.label), spin);
        attachTip(row, QString::fromUtf8(it.tip));
        m_card->contentLayout()->addWidget(row);
        m_spins.push_back(spin);

        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [it](double v) {
                    it.setter(v);
                    ConfigBridge::instance().markDirty();
                });
    }

    m_card->contentLayout()->addWidget(makeHint(QString::fromUtf8(
        u8"★ 宽高比两项会在读配置时自动排序（min ≤ max），所以乱填不会把自己锁死。\n"
        u8"★ 值同时也写进 config.ini 的 [target_stabilizer] 段，手改文件也认。")));
}

// 从运行时配置回填控件。★ 必须 blockSignals: 否则回填会触发 setter →
// configChanged → 又回到这里, 变成回路; 也会把"正在编辑的值"覆盖掉。
void StabilizerPage::reloadFromRuntime()
{
    auto& cm = ConfigManager::instance();

    const double values[] = {
        cm.targetHysteresisRatio(),
        cm.targetMaxDistancePx(),
        cm.targetMatchCenterRatio(),
        cm.targetAreaRatioTol(),
        cm.targetKSnapMult(),
        cm.targetMinAspect(),
        cm.targetMaxAspect(),
    };

    for (size_t i = 0; i < m_spins.size() && i < std::size(values); ++i)
    {
        QDoubleSpinBox* spin = m_spins[i];
        if (!spin) continue;
        // ★ 正在被编辑(有焦点)的那个不回填, 免得抢用户正在敲的数字。
        if (spin->hasFocus()) continue;
        if (qFuzzyCompare(spin->value(), values[i])) continue;

        const QSignalBlocker block(spin);
        spin->setValue(values[i]);
    }
}
