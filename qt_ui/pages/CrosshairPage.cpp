#include "pages/CrosshairPage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include "crosshair/color_picker.h"

#include <algorithm>

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {

struct PresetDef {
    const char* label;
    QString nameA;
    int hLoA, hHiA, sLoA, sHiA, vLoA, vHiA;
    bool hasB;
    QString nameB;
    int hLoB, hHiB, sLoB, sHiB, vLoB, vHiB;
};

const PresetDef kPresets[] = {
    { "红色（双区间）", "Red-Low", 0, 10, 120, 255, 120, 255, true, "Red-High", 160, 179, 120, 255, 120, 255 },
    { "绿色（荧光/鲜绿）", "Green", 40, 85, 100, 255, 100, 255, false, "", 0, 0, 0, 0, 0, 0 },
    { "青色（浅蓝/天青）", "Cyan", 85, 100, 90, 255, 100, 255, false, "", 0, 0, 0, 0, 0, 0 },
    { "紫色（粉紫/亮紫）", "Purple", 125, 155, 90, 255, 100, 255, false, "", 0, 0, 0, 0, 0, 0 },
    { "黄色（明黄/金黄）", "Yellow", 20, 35, 120, 255, 120, 255, false, "", 0, 0, 0, 0, 0, 0 },
    { "白色（高亮准星）", "White", 0, 179, 0, 35, 200, 255, false, "", 0, 0, 0, 0, 0, 0 },
};
constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

QColor computePreviewColor(int hLo, int hHi, int sLo, int sHi, int vLo, int vHi) {
    int midH = (hLo + hHi) / 2;
    int midS = (sLo + sHi) / 2;
    int midV = (vLo + vHi) / 2;
    int hDeg = std::clamp(midH * 2, 0, 359);
    int sVal = std::clamp(midS, 0, 255);
    int vVal = std::clamp(midV, 0, 255);
    return QColor::fromHsv(hDeg, sVal, vVal);
}

}

CrosshairPage::CrosshairPage(QWidget* parent, bool laserMode)
    : QWidget(parent), m_laserMode(laserMode) {
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outerLayout->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    auto& cfg = ConfigManager::instance();

    auto* regionCard = new CardWidget(
        m_laserMode ? QStringLiteral("镭射取样区域") : QStringLiteral("取样区域"),
        QStringLiteral("color-swatch"));

    QSlider* wSlider = nullptr;
    regionCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("宽度（px）"),
            4, m_laserMode ? 4096 : 256,
            m_laserMode ? cfg.laserRectW() : cfg.crosshairRectW(), wSlider, m_rectW));
    connect(m_rectW, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (m_laserMode) ConfigManager::instance().setLaserRectW(v);
                              else ConfigManager::instance().setCrosshairRectW(v); });

    QSlider* hSlider = nullptr;
    regionCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("高度（px）"),
            4, m_laserMode ? 4096 : 256,
            m_laserMode ? cfg.laserRectH() : cfg.crosshairRectH(), hSlider, m_rectH));
    connect(m_rectH, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (m_laserMode) ConfigManager::instance().setLaserRectH(v);
                              else ConfigManager::instance().setCrosshairRectH(v); });

    if (!m_laserMode) {
        QSlider* offsetSlider = nullptr;
        regionCard->contentLayout()->addWidget(
            FormKit::sliderRow(QStringLiteral("垂直偏移（px，正数向下）"),
                               -2048, 2048, cfg.crosshairOffsetY(), offsetSlider, m_offsetY));
        connect(m_offsetY, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [](int v) { ConfigManager::instance().setCrosshairOffsetY(v); });
    }

    if (m_laserMode) {
        auto addInt = [this, regionCard](const QString& label, int value, int min, int max,
                                         QSpinBox*& spin, void (ConfigManager::*setter)(int)) {
            spin = new QSpinBox(this);
            spin->setRange(min, max);
            spin->setValue(value);
            regionCard->contentLayout()->addWidget(FormKit::fieldRow(label, spin));
            connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                    [setter](int v) { (ConfigManager::instance().*setter)(v); });
        };
        addInt(QStringLiteral("取样中心 X（检测画面 px）"), cfg.laserCenterX(), 0, 8192,
               m_laserCenterX, &ConfigManager::setLaserCenterX);
        addInt(QStringLiteral("取样中心 Y（检测画面 px）"), cfg.laserCenterY(), 0, 8192,
               m_laserCenterY, &ConfigManager::setLaserCenterY);
        addInt(QStringLiteral("瞄点区域中心 X"), cfg.laserTargetCenterX(), 0, 8192,
               m_laserTargetCenterX, &ConfigManager::setLaserTargetCenterX);
        addInt(QStringLiteral("瞄点区域中心 Y"), cfg.laserTargetCenterY(), 0, 8192,
               m_laserTargetCenterY, &ConfigManager::setLaserTargetCenterY);
        addInt(QStringLiteral("瞄点区域宽度"), cfg.laserTargetRectW(), 4, 4096,
               m_laserTargetRectW, &ConfigManager::setLaserTargetRectW);
        addInt(QStringLiteral("瞄点区域高度"), cfg.laserTargetRectH(), 4, 4096,
               m_laserTargetRectH, &ConfigManager::setLaserTargetRectH);
    }

    layout->addWidget(regionCard);

    auto* colorCard = new CardWidget(
        m_laserMode ? QStringLiteral("镭射颜色") : QStringLiteral("准星颜色"),
        QStringLiteral("palette"));

    auto* toolBar = new QHBoxLayout;
    toolBar->setSpacing(8);

    m_presetCombo = new QComboBox(this);
    for (int i = 0; i < kPresetCount; ++i) {
        m_presetCombo->addItem(QString::fromUtf8(kPresets[i].label));
    }
    m_presetCombo->setMinimumHeight(32);
    toolBar->addWidget(m_presetCombo, 1);

    m_addPresetBtn = new QPushButton(QStringLiteral("应用预设"), this);
    m_addPresetBtn->setCursor(Qt::PointingHandCursor);
    m_addPresetBtn->setMinimumHeight(32);
    m_addPresetBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:#F4F5F7; color:#17191F; border:1px solid rgba(0,0,0,0.08);"
        " border-radius:6px; padding:4px 12px; font-size:12px; font-weight:500;}"
        "QPushButton:hover{background:#EAEAED; border-color:rgba(0,0,0,0.15);}"));
    toolBar->addWidget(m_addPresetBtn);

    m_pickColorBtn = new QPushButton(QStringLiteral("屏幕取色"), this);
    m_pickColorBtn->setCursor(Qt::PointingHandCursor);
    m_pickColorBtn->setMinimumHeight(32);
    m_pickColorBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:#EEF0FB; color:#5E6AD2; border:1px solid rgba(94,106,210,0.25);"
        " border-radius:6px; padding:4px 14px; font-size:12px; font-weight:600;}"
        "QPushButton:hover{background:#E0E4F9; border-color:#5E6AD2;}"));
    toolBar->addWidget(m_pickColorBtn);

    m_addColorBtn = new QPushButton(QStringLiteral("+ 自定义"), this);
    m_addColorBtn->setCursor(Qt::PointingHandCursor);
    m_addColorBtn->setMinimumHeight(32);
    m_addColorBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:#FFFFFF; color:#3C3C44; border:1px dashed rgba(0,0,0,0.18);"
        " border-radius:6px; padding:4px 12px; font-size:12px; font-weight:500;}"
        "QPushButton:hover{color:#5E6AD2; border-color:#5E6AD2; background:#FAFAFC;}"));
    toolBar->addWidget(m_addColorBtn);

    colorCard->contentLayout()->addLayout(toolBar);

    m_colorListContainer = new QWidget(this);
    m_colorListLayout = new QVBoxLayout(m_colorListContainer);
    m_colorListLayout->setContentsMargins(0, 6, 0, 0);
    m_colorListLayout->setSpacing(8);
    colorCard->contentLayout()->addWidget(m_colorListContainer);

    layout->addWidget(colorCard);

    connect(m_addPresetBtn, &QPushButton::clicked, this, [this]() {
        addPreset(m_presetCombo->currentIndex());
    });
    connect(m_addColorBtn, &QPushButton::clicked, this, &CrosshairPage::addNewColor);
    connect(m_pickColorBtn, &QPushButton::clicked, this, &CrosshairPage::toggleColorPick);

    m_pickTimer = new QTimer(this);
    m_pickTimer->setInterval(120);
    connect(m_pickTimer, &QTimer::timeout, this, &CrosshairPage::pollPickedColor);

    auto* shapeCard = new CardWidget(
        m_laserMode ? QStringLiteral("镭射找色参数") : QStringLiteral("找色参数"),
        QStringLiteral("target"));

    QSlider* mpSlider = nullptr;
    shapeCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("最小像素阈值"),
            1, m_laserMode ? 10000 : 200,
            m_laserMode ? cfg.laserMinPixelCount() : cfg.crosshairMinPixelCount(), mpSlider, m_minPixels));
    connect(m_minPixels, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (m_laserMode) ConfigManager::instance().setLaserMinPixelCount(v);
                              else ConfigManager::instance().setCrosshairMinPixelCount(v); });

    QSlider* crSlider = nullptr;
    shapeCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("闭合滤波半径"),
            0, m_laserMode ? 9 : 7,
            m_laserMode ? cfg.laserCloseRadius() : cfg.crosshairCloseRadius(), crSlider, m_closeRadius));
    connect(m_closeRadius, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (m_laserMode) ConfigManager::instance().setLaserCloseRadius(v);
                              else ConfigManager::instance().setCrosshairCloseRadius(v); });

    if (m_laserMode) {
        m_laserElongation = new QDoubleSpinBox(this);
        m_laserElongation->setRange(1.0, 30.0);
        m_laserElongation->setSingleStep(0.1);
        m_laserElongation->setValue(cfg.laserMinElongation());
        shapeCard->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("最小线条长宽比"), m_laserElongation));
        connect(m_laserElongation, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [](double v) { ConfigManager::instance().setLaserMinElongation(static_cast<float>(v)); });
        m_laserSmooth = new QDoubleSpinBox(this);
        m_laserSmooth->setRange(0.0, 1.0);
        m_laserSmooth->setSingleStep(0.05);
        m_laserSmooth->setDecimals(2);
        m_laserSmooth->setValue(cfg.laserSmooth());
        shapeCard->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("端点平滑强度"), m_laserSmooth));
        connect(m_laserSmooth, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [](double v) { ConfigManager::instance().setLaserSmooth(static_cast<float>(v)); });
    }

    layout->addWidget(shapeCard);

    layout->addStretch();

    loadConfig();
    connect(&cfg, &ConfigManager::configLoaded, this, &CrosshairPage::loadConfig);
}

void CrosshairPage::loadConfig() {
    auto& cfg = ConfigManager::instance();
    const QSignalBlocker blockWidth(m_rectW), blockHeight(m_rectH);
    const QSignalBlocker blockPixels(m_minPixels), blockRadius(m_closeRadius);

    m_rectW->setValue(m_laserMode ? cfg.laserRectW() : cfg.crosshairRectW());
    m_rectH->setValue(m_laserMode ? cfg.laserRectH() : cfg.crosshairRectH());
    if (m_offsetY) {
        const QSignalBlocker blockOffset(m_offsetY);
        m_offsetY->setValue(cfg.crosshairOffsetY());
    }
    m_minPixels->setValue(m_laserMode ? cfg.laserMinPixelCount() : cfg.crosshairMinPixelCount());
    m_closeRadius->setValue(m_laserMode ? cfg.laserCloseRadius() : cfg.crosshairCloseRadius());
    if (m_laserMode) {
        const auto set = [](auto* spin, auto value) { const QSignalBlocker blocker(spin); spin->setValue(value); };
        set(m_laserCenterX, cfg.laserCenterX());
        set(m_laserCenterY, cfg.laserCenterY());
        set(m_laserTargetCenterX, cfg.laserTargetCenterX());
        set(m_laserTargetCenterY, cfg.laserTargetCenterY());
        set(m_laserTargetRectW, cfg.laserTargetRectW());
        set(m_laserTargetRectH, cfg.laserTargetRectH());
        set(m_laserElongation, cfg.laserMinElongation());
        set(m_laserSmooth, cfg.laserSmooth());
    }

    m_colors = m_laserMode ? cfg.laserColors() : cfg.crosshairColors();
    rebuildColorList();
}

void CrosshairPage::rebuildColorList() {
    QLayoutItem* item = nullptr;
    while ((item = m_colorListLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    if (m_colors.isEmpty()) {
        auto* emptyLabel = new QLabel(m_laserMode
            ? QStringLiteral("暂未配置镭射颜色，可从上方选择预设或点击屏幕取色添加。")
            : QStringLiteral("暂未配置准星颜色，可从上方选择预设或点击屏幕取色添加。"), m_colorListContainer);
        emptyLabel->setStyleSheet(QStringLiteral("color:#98A1B0; font-size:12px; padding:12px;"));
        emptyLabel->setAlignment(Qt::AlignCenter);
        m_colorListLayout->addWidget(emptyLabel);
        return;
    }

    const QString spinBoxSS = QStringLiteral(
        "QSpinBox{background:#FFFFFF; border:1px solid #E4E4E7; border-radius:4px;"
        " padding:1px 3px; font-size:12px; font-weight:500; color:#17191F;}"
        "QSpinBox:focus{border-color:#5E6AD2;}"
        "QSpinBox::up-button, QSpinBox::down-button{width:0px;}");

    for (int i = 0; i < m_colors.size(); ++i) {
        const int idx = i;
        auto& c = m_colors[idx];

        auto* itemFrame = new QFrame(m_colorListContainer);
        itemFrame->setObjectName(QStringLiteral("colorItemFrame"));
        itemFrame->setStyleSheet(QStringLiteral(
            "QFrame#colorItemFrame{background:#FAFAFB; border:1px solid rgba(0,0,0,0.06);"
            " border-radius:8px;}"
            "QFrame#colorItemFrame:hover{border-color:rgba(94,106,210,0.35);}"));

        auto* frameLayout = new QVBoxLayout(itemFrame);
        frameLayout->setContentsMargins(12, 10, 12, 10);
        frameLayout->setSpacing(8);

        auto* headerRow = new QHBoxLayout;
        headerRow->setSpacing(10);

        auto* chk = new QCheckBox(itemFrame);
        chk->setChecked(c.enabled);
        chk->setToolTip(QStringLiteral("启用/禁用该颜色匹配"));
        chk->setCursor(Qt::PointingHandCursor);
        headerRow->addWidget(chk);

        auto* colorDot = new QFrame(itemFrame);
        colorDot->setFixedSize(18, 18);
        auto updateDotColor = [colorDot](int hLo, int hHi, int sLo, int sHi, int vLo, int vHi) {
            QColor qc = computePreviewColor(hLo, hHi, sLo, sHi, vLo, vHi);
            colorDot->setStyleSheet(QStringLiteral(
                "background-color:%1; border:1px solid rgba(0,0,0,0.2); border-radius:9px;")
                .arg(qc.name()));
        };
        updateDotColor(c.hLow, c.hHigh, c.sMin, c.sMax, c.vMin, c.vMax);
        headerRow->addWidget(colorDot);

        auto* nameEdit = new QLineEdit(c.name, itemFrame);
        nameEdit->setPlaceholderText(QStringLiteral("颜色名称"));
        nameEdit->setStyleSheet(QStringLiteral(
            "QLineEdit{background:#FFFFFF; border:1px solid #E4E4E7; border-radius:4px;"
            " padding:2px 8px; font-size:12px; font-weight:500; color:#17191F;}"
            "QLineEdit:focus{border-color:#5E6AD2;}"));
        headerRow->addWidget(nameEdit, 1);

        auto* delBtn = new QPushButton(QStringLiteral("删除"), itemFrame);
        delBtn->setObjectName(QStringLiteral("removeColorButton"));
        delBtn->setFixedSize(46, 26);
        delBtn->setCursor(Qt::PointingHandCursor);
        delBtn->setToolTip(QStringLiteral("移除该颜色"));
        delBtn->setStyleSheet(QStringLiteral(
            "QPushButton#removeColorButton{color:#B4232F; background-color:#FFF1F2;"
            " border:1px solid #F4B7BD; border-radius:5px; padding:0;"
            " font-size:12px; font-weight:600;}"
            "QPushButton#removeColorButton:hover{color:#FFFFFF;"
            " background-color:#D23B3B; border-color:#D23B3B;}"));
        headerRow->addWidget(delBtn);

        frameLayout->addLayout(headerRow);

        auto* rangesRow = new QHBoxLayout;
        rangesRow->setSpacing(14);

        auto makeChannelGroup = [&](const QString& tag, const QString& tagColor,
                                    int minVal, int maxVal, int curLo, int curHi,
                                    QSpinBox*& loSpin, QSpinBox*& hiSpin) {
            auto* grp = new QHBoxLayout;
            grp->setSpacing(4);

            auto* tagLbl = new QLabel(tag, itemFrame);
            tagLbl->setStyleSheet(QStringLiteral(
                "color:%1; font-weight:bold; font-size:12px; min-width:14px;").arg(tagColor));
            grp->addWidget(tagLbl);

            loSpin = new QSpinBox(itemFrame);
            loSpin->setRange(minVal, maxVal);
            loSpin->setValue(curLo);
            loSpin->setFixedWidth(52);
            loSpin->setFixedHeight(26);
            loSpin->setAlignment(Qt::AlignCenter);
            loSpin->setStyleSheet(spinBoxSS);
            grp->addWidget(loSpin);

            auto* sep = new QLabel(QStringLiteral("~"), itemFrame);
            sep->setStyleSheet(QStringLiteral("color:#A1A1AA; font-weight:bold; font-size:12px;"));
            grp->addWidget(sep);

            hiSpin = new QSpinBox(itemFrame);
            hiSpin->setRange(minVal, maxVal);
            hiSpin->setValue(curHi);
            hiSpin->setFixedWidth(52);
            hiSpin->setFixedHeight(26);
            hiSpin->setAlignment(Qt::AlignCenter);
            hiSpin->setStyleSheet(spinBoxSS);
            grp->addWidget(hiSpin);

            rangesRow->addLayout(grp);
        };

        QSpinBox *hLoBox = nullptr, *hHiBox = nullptr;
        QSpinBox *sLoBox = nullptr, *sHiBox = nullptr;
        QSpinBox *vLoBox = nullptr, *vHiBox = nullptr;

        makeChannelGroup(QStringLiteral("H"), QStringLiteral("#5E6AD2"), 0, 179, c.hLow, c.hHigh, hLoBox, hHiBox);
        makeChannelGroup(QStringLiteral("S"), QStringLiteral("#2CA02C"), 0, 255, c.sMin, c.sMax, sLoBox, sHiBox);
        makeChannelGroup(QStringLiteral("V"), QStringLiteral("#D97706"), 0, 255, c.vMin, c.vMax, vLoBox, vHiBox);
        rangesRow->addStretch();

        frameLayout->addLayout(rangesRow);

        m_colorListLayout->addWidget(itemFrame);

        auto onValueChanged = [this, idx, chk, nameEdit, hLoBox, hHiBox, sLoBox, sHiBox, vLoBox, vHiBox, updateDotColor]() {
            if (idx < 0 || idx >= m_colors.size()) return;
            auto& entry = m_colors[idx];
            entry.enabled = chk->isChecked();
            entry.name = nameEdit->text().trimmed();
            entry.hLow = hLoBox->value();
            entry.hHigh = hHiBox->value();
            entry.sMin = sLoBox->value();
            entry.sMax = sHiBox->value();
            entry.vMin = vLoBox->value();
            entry.vMax = vHiBox->value();
            updateDotColor(entry.hLow, entry.hHigh, entry.sMin, entry.sMax, entry.vMin, entry.vMax);
            saveCrosshairColors();
        };

        connect(chk, &QCheckBox::toggled, this, onValueChanged);
        connect(nameEdit, &QLineEdit::editingFinished, this, onValueChanged);
        connect(hLoBox, QOverload<int>::of(&QSpinBox::valueChanged), this, onValueChanged);
        connect(hHiBox, QOverload<int>::of(&QSpinBox::valueChanged), this, onValueChanged);
        connect(sLoBox, QOverload<int>::of(&QSpinBox::valueChanged), this, onValueChanged);
        connect(sHiBox, QOverload<int>::of(&QSpinBox::valueChanged), this, onValueChanged);
        connect(vLoBox, QOverload<int>::of(&QSpinBox::valueChanged), this, onValueChanged);
        connect(vHiBox, QOverload<int>::of(&QSpinBox::valueChanged), this, onValueChanged);

        connect(delBtn, &QPushButton::clicked, this, [this, idx]() {
            removeColorAt(idx);
        });
    }
}

void CrosshairPage::saveCrosshairColors() {
    if (m_laserMode) ConfigManager::instance().setLaserColors(m_colors);
    else ConfigManager::instance().setCrosshairColors(m_colors);
}

void CrosshairPage::addPreset(int presetIdx) {
    if (presetIdx < 0 || presetIdx >= kPresetCount) return;
    const auto& p = kPresets[presetIdx];

    ConfigManager::ColorProfile cA;
    cA.name = p.nameA;
    cA.enabled = true;
    cA.hLow = p.hLoA;
    cA.hHigh = p.hHiA;
    cA.sMin = p.sLoA;
    cA.sMax = p.sHiA;
    cA.vMin = p.vLoA;
    cA.vMax = p.vHiA;
    m_colors.append(cA);

    if (p.hasB) {
        ConfigManager::ColorProfile cB;
        cB.name = p.nameB;
        cB.enabled = true;
        cB.hLow = p.hLoB;
        cB.hHigh = p.hHiB;
        cB.sMin = p.sLoB;
        cB.sMax = p.sHiB;
        cB.vMin = p.vLoB;
        cB.vMax = p.vHiB;
        m_colors.append(cB);
    }

    rebuildColorList();
    saveCrosshairColors();
}

void CrosshairPage::addNewColor() {
    ConfigManager::ColorProfile c;
    c.name = QStringLiteral("自定义 %1").arg(m_colors.size() + 1);
    c.enabled = true;
    c.hLow = m_laserMode ? 0 : 40;
    c.hHigh = m_laserMode ? 10 : 85;
    c.sMin = 100;
    c.sMax = 255;
    c.vMin = 100;
    c.vMax = 255;
    m_colors.append(c);

    rebuildColorList();
    saveCrosshairColors();
}

void CrosshairPage::removeColorAt(int index) {
    if (index >= 0 && index < m_colors.size()) {
        m_colors.removeAt(index);
        rebuildColorList();
        saveCrosshairColors();
    }
}

void CrosshairPage::toggleColorPick() {
    if (m_pickToken != 0) {
        crosshair::CancelColorPick();
        finishPicking();
        return;
    }
    auto& cm = ConfigManager::instance();
    if (!cm.showWindow())
        cm.setShowWindow(true);

    m_pickToken = crosshair::ArmColorPick(0);
    m_pickColorBtn->setText(QStringLiteral("取消取色"));
    m_pickColorBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:#FDF2F2; color:#D23B3B; border:1px solid #D23B3B;"
        " border-radius:6px; padding:4px 14px; font-size:12px; font-weight:600;}"));
    m_pickTimer->start();
}

void CrosshairPage::pollPickedColor() {
    int h = 0, s = 0, v = 0;
    if (crosshair::TakePickedColor(m_pickToken, h, s, v)) {
        applyPickedColor(h, s, v);
        finishPicking();
    } else if (crosshair::ArmedToken() != m_pickToken) {
        finishPicking();
    }
}

void CrosshairPage::applyPickedColor(int h, int s, int v) {
    constexpr int kHueHalf = 15;
    constexpr int kSvMargin = 70;
    const int sLo = std::max(0, s - kSvMargin);
    const int vLo = std::max(0, v - kSvMargin);
    const QString base = QStringLiteral("取色 H%1 S%2 V%3").arg(h).arg(s).arg(v);

    const int lo = h - kHueHalf;
    const int hi = h + kHueHalf;

    if (lo < 0) {
        ConfigManager::ColorProfile c1;
        c1.name = base + QStringLiteral(" 低");
        c1.enabled = true;
        c1.hLow = 0; c1.hHigh = hi;
        c1.sMin = sLo; c1.sMax = 255;
        c1.vMin = vLo; c1.vMax = 255;
        m_colors.append(c1);

        ConfigManager::ColorProfile c2;
        c2.name = base + QStringLiteral(" 高");
        c2.enabled = true;
        c2.hLow = 180 + lo; c2.hHigh = 179;
        c2.sMin = sLo; c2.sMax = 255;
        c2.vMin = vLo; c2.vMax = 255;
        m_colors.append(c2);
    } else if (hi > 179) {
        ConfigManager::ColorProfile c1;
        c1.name = base + QStringLiteral(" 低");
        c1.enabled = true;
        c1.hLow = 0; c1.hHigh = hi - 180;
        c1.sMin = sLo; c1.sMax = 255;
        c1.vMin = vLo; c1.vMax = 255;
        m_colors.append(c1);

        ConfigManager::ColorProfile c2;
        c2.name = base + QStringLiteral(" 高");
        c2.enabled = true;
        c2.hLow = lo; c2.hHigh = 179;
        c2.sMin = sLo; c2.sMax = 255;
        c2.vMin = vLo; c2.vMax = 255;
        m_colors.append(c2);
    } else {
        ConfigManager::ColorProfile c;
        c.name = base;
        c.enabled = true;
        c.hLow = lo; c.hHigh = hi;
        c.sMin = sLo; c.sMax = 255;
        c.vMin = vLo; c.vMax = 255;
        m_colors.append(c);
    }

    rebuildColorList();
    saveCrosshairColors();
}

void CrosshairPage::finishPicking() {
    m_pickToken = 0;
    if (m_pickTimer)
        m_pickTimer->stop();
    m_pickColorBtn->setText(QStringLiteral("屏幕取色"));
    m_pickColorBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:#EEF0FB; color:#5E6AD2; border:1px solid rgba(94,106,210,0.25);"
        " border-radius:6px; padding:4px 14px; font-size:12px; font-weight:600;}"
        "QPushButton:hover{background:#E0E4F9; border-color:#5E6AD2;}"));
}
