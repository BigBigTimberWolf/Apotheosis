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
#include <QStringList>
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
        m_algorithm = new QComboBox(this);
        m_algorithm->addItems({QStringLiteral("原有算法"), QStringLiteral("质心（局部筛选）")});
        m_algorithm->setCurrentIndex(cfg.crosshairAlgorithm());
        m_algorithm->setToolTip(QStringLiteral("质心：先筛选准星附近的局部色块，排除大面积色块和搜索框边缘残片，再求中心。最小像素数生效（至少2个），闭合滤波仅用于原有算法。两种算法均与目标使用同一帧画面；未找到时最多沿用上次位置3帧，第4帧回到中心。"));
        regionCard->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("准星找色算法"), m_algorithm));
        connect(m_algorithm, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [](int v) { ConfigManager::instance().setCrosshairAlgorithm(v); });
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
        "QPushButton{background:#222226; color:#F0EDE6; border:1px solid rgba(213,181,107,0.08);"
        " border-radius:6px; padding:4px 12px; font-size:12px; font-weight:500;}"
        "QPushButton:hover{background:#2B2923; border-color:rgba(213,181,107,0.15);}"));
    toolBar->addWidget(m_addPresetBtn);

    m_pickColorBtn = new QPushButton(QStringLiteral("屏幕取色"), this);
    m_pickColorBtn->setCursor(Qt::PointingHandCursor);
    m_pickColorBtn->setToolTip(QStringLiteral("在检测预览中移动鼠标，点击画面锁定放大镜，再点击放大镜中的单个像素块取色；右键取消。新颜色的 HSV 范围可在下方调整。"));
    m_pickColorBtn->setMinimumHeight(32);
    m_pickColorBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:#302A1E; color:#D5B56B; border:1px solid rgba(213,181,107,0.25);"
        " border-radius:6px; padding:4px 14px; font-size:12px; font-weight:600;}"
        "QPushButton:hover{background:#3A3020; border-color:#D5B56B;}"));
    toolBar->addWidget(m_pickColorBtn);

    m_addColorBtn = new QPushButton(QStringLiteral("+ 自定义"), this);
    m_addColorBtn->setCursor(Qt::PointingHandCursor);
    m_addColorBtn->setMinimumHeight(32);
    m_addColorBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:#19191C; color:#DCD7CA; border:1px dashed rgba(213,181,107,0.18);"
        " border-radius:6px; padding:4px 12px; font-size:12px; font-weight:500;}"
        "QPushButton:hover{color:#D5B56B; border-color:#D5B56B; background:#242321;}"));
    toolBar->addWidget(m_addColorBtn);

    colorCard->contentLayout()->addLayout(toolBar);

    m_colorListContainer = new QWidget(this);
    m_colorListLayout = new QVBoxLayout(m_colorListContainer);
    m_colorListLayout->setContentsMargins(0, 6, 0, 0);
    m_colorListLayout->setSpacing(8);
    colorCard->contentLayout()->addWidget(m_colorListContainer);

    layout->addWidget(colorCard);

    auto* labCard = new CardWidget(QStringLiteral("取色实验室"), QStringLiteral("color-swatch"));
    auto* labButtons = new QHBoxLayout;
    labButtons->setSpacing(8);
    m_labTargetBtn = new QPushButton(QStringLiteral("采目标色"), this);
    m_labBackgroundBtn = new QPushButton(QStringLiteral("采背景色"), this);
    m_labApplyBtn = new QPushButton(QStringLiteral("加入颜色列表"), this);
    for (auto* button : {m_labTargetBtn, m_labBackgroundBtn, m_labApplyBtn}) {
        button->setMinimumHeight(32);
        button->setCursor(Qt::PointingHandCursor);
        labButtons->addWidget(button);
    }
    m_labApplyBtn->setEnabled(false);
    labCard->contentLayout()->addLayout(labButtons);
    m_labPreviewToggle = new QCheckBox(QStringLiteral("在检测预览中实时显示匹配像素"), this);
    labCard->contentLayout()->addWidget(m_labPreviewToggle);
    m_labSummary = new QLabel(QStringLiteral("先采目标色，再采容易误识别的背景色。"), this);
    m_labSummary->setWordWrap(true);
    m_labSummary->setStyleSheet(QStringLiteral("color:#ABA697; font-size:12px;"));
    labCard->contentLayout()->addWidget(m_labSummary);
    auto* clearLabBtn = new QPushButton(QStringLiteral("清空本次样本"), this);
    clearLabBtn->setCursor(Qt::PointingHandCursor);
    labCard->contentLayout()->addWidget(clearLabBtn);
    layout->addWidget(labCard);

    connect(m_labTargetBtn, &QPushButton::clicked, this,
            [this]() { startColorPick(PickRole::TargetSample); });
    connect(m_labBackgroundBtn, &QPushButton::clicked, this,
            [this]() { startColorPick(PickRole::BackgroundSample); });
    connect(m_labApplyBtn, &QPushButton::clicked, this, &CrosshairPage::applyLabProfile);
    connect(clearLabBtn, &QPushButton::clicked, this, &CrosshairPage::clearLabSamples);
    connect(m_labPreviewToggle, &QCheckBox::toggled, this, &CrosshairPage::updateLabPreview);

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

CrosshairPage::~CrosshairPage() {
    if (m_pickToken != 0 && crosshair::ArmedToken() == m_pickToken)
        crosshair::CancelColorPick();
    crosshair::SetColorLabPreview({});
}

void CrosshairPage::loadConfig() {
    if (m_algorithm) {
        const QSignalBlocker blocker(m_algorithm);
        m_algorithm->setCurrentIndex(ConfigManager::instance().crosshairAlgorithm());
    }
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
        emptyLabel->setStyleSheet(QStringLiteral("color:#8E887A; font-size:12px; padding:12px;"));
        emptyLabel->setAlignment(Qt::AlignCenter);
        m_colorListLayout->addWidget(emptyLabel);
        return;
    }

    const QString spinBoxSS = QStringLiteral(
        "QSpinBox{background:#19191C; border:1px solid #35332D; border-radius:4px;"
        " padding:1px 3px; font-size:12px; font-weight:500; color:#F0EDE6;}"
        "QSpinBox:focus{border-color:#D5B56B;}"
        "QSpinBox::up-button, QSpinBox::down-button{width:0px;}");

    for (int i = 0; i < m_colors.size(); ++i) {
        const int idx = i;
        auto& c = m_colors[idx];

        auto* itemFrame = new QFrame(m_colorListContainer);
        itemFrame->setObjectName(QStringLiteral("colorItemFrame"));
        itemFrame->setStyleSheet(QStringLiteral(
            "QFrame#colorItemFrame{background:#202023; border:1px solid rgba(213,181,107,0.06);"
            " border-radius:8px;}"
            "QFrame#colorItemFrame:hover{border-color:rgba(213,181,107,0.35);}"));

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
                "background-color:%1; border:1px solid rgba(213,181,107,0.2); border-radius:9px;")
                .arg(qc.name()));
        };
        updateDotColor(c.hLow, c.hHigh, c.sMin, c.sMax, c.vMin, c.vMax);
        headerRow->addWidget(colorDot);

        auto* nameEdit = new QLineEdit(c.name, itemFrame);
        nameEdit->setPlaceholderText(QStringLiteral("颜色名称"));
        nameEdit->setStyleSheet(QStringLiteral(
            "QLineEdit{background:#19191C; border:1px solid #35332D; border-radius:4px;"
            " padding:2px 8px; font-size:12px; font-weight:500; color:#F0EDE6;}"
            "QLineEdit:focus{border-color:#D5B56B;}"));
        headerRow->addWidget(nameEdit, 1);

        auto* exactChk = new QCheckBox(QStringLiteral("精确 HSV"), itemFrame);
        exactChk->setChecked(c.exactHsv);
        exactChk->setToolTip(QStringLiteral("严格使用这一行的 H/S/V 范围；实验室生成的准星颜色默认启用。"));
        exactChk->setVisible(!m_laserMode);
        headerRow->addWidget(exactChk);

        auto* delBtn = new QPushButton(QStringLiteral("删除"), itemFrame);
        delBtn->setObjectName(QStringLiteral("removeColorButton"));
        delBtn->setFixedSize(46, 26);
        delBtn->setCursor(Qt::PointingHandCursor);
        delBtn->setToolTip(QStringLiteral("移除该颜色"));
        delBtn->setStyleSheet(QStringLiteral(
            "QPushButton#removeColorButton{color:#F28D98; background-color:#321E21;"
            " border:1px solid #693139; border-radius:5px; padding:0;"
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
            sep->setStyleSheet(QStringLiteral("color:#A49E90; font-weight:bold; font-size:12px;"));
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

        makeChannelGroup(QStringLiteral("H"), QStringLiteral("#D5B56B"), 0, 179, c.hLow, c.hHigh, hLoBox, hHiBox);
        makeChannelGroup(QStringLiteral("S"), QStringLiteral("#2CA02C"), 0, 255, c.sMin, c.sMax, sLoBox, sHiBox);
        makeChannelGroup(QStringLiteral("V"), QStringLiteral("#D97706"), 0, 255, c.vMin, c.vMax, vLoBox, vHiBox);
        rangesRow->addStretch();

        frameLayout->addLayout(rangesRow);

        m_colorListLayout->addWidget(itemFrame);

        auto onValueChanged = [this, idx, chk, exactChk, nameEdit, hLoBox, hHiBox, sLoBox, sHiBox, vLoBox, vHiBox, updateDotColor]() {
            if (idx < 0 || idx >= m_colors.size()) return;
            auto& entry = m_colors[idx];
            entry.enabled = chk->isChecked();
            entry.exactHsv = exactChk->isChecked();
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
        connect(exactChk, &QCheckBox::toggled, this, onValueChanged);
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
    startColorPick(PickRole::Direct);
}

void CrosshairPage::startColorPick(PickRole role) {
    if (m_pickToken != 0 && m_pickRole == role) {
        crosshair::CancelColorPick();
        finishPicking();
        return;
    }
    if (m_pickToken != 0) {
        crosshair::CancelColorPick();
        finishPicking();
    }
    auto& cm = ConfigManager::instance();
    if (!cm.showWindow())
        cm.setShowWindow(true);

    m_pickRole = role;
    m_pickToken = crosshair::ArmColorPick(0);
    if (role == PickRole::Direct) m_pickColorBtn->setText(QStringLiteral("取消取色"));
    if (role == PickRole::TargetSample) m_labTargetBtn->setText(QStringLiteral("取消目标采样"));
    if (role == PickRole::BackgroundSample) m_labBackgroundBtn->setText(QStringLiteral("取消背景采样"));
    m_pickTimer->start();
}

void CrosshairPage::pollPickedColor() {
    int h = 0, s = 0, v = 0;
    if (crosshair::TakePickedColor(m_pickToken, h, s, v)) {
        if (m_pickRole == PickRole::Direct) {
            applyPickedColor(h, s, v);
        } else {
            auto& samples = m_pickRole == PickRole::TargetSample
                ? m_labTargets : m_labBackgrounds;
            if (samples.size() >= 32) samples.erase(samples.begin());
            samples.push_back({h, s, v});
            updateLabPreview();
        }
        finishPicking();
    } else if (crosshair::ArmedToken() != m_pickToken) {
        finishPicking();
    }
}

void CrosshairPage::updateLabPreview() {
    const auto result = crosshair::deriveColorLabBands(m_labTargets, m_labBackgrounds);
    m_labApplyBtn->setEnabled(!result.bands.empty());
    if (m_labTargets.empty()) {
        m_labSummary->setText(QStringLiteral("先采目标色，再采容易误识别的背景色。"));
    } else {
        QStringList ranges;
        for (const auto& band : result.bands) {
            ranges << QStringLiteral("H %1–%2 · S %3–%4 · V %5–%6")
                .arg(band.h_low).arg(band.h_high)
                .arg(band.s_min).arg(band.s_max)
                .arg(band.v_min).arg(band.v_max);
        }
        m_labSummary->setText(
            QStringLiteral("目标样本 %1，背景样本 %2；背景误命中 %3。候选范围：%4%5")
                .arg(m_labTargets.size()).arg(m_labBackgrounds.size())
                .arg(result.background_hits).arg(ranges.join(QStringLiteral(" / ")))
                .arg(result.background_hits > 0
                    ? QStringLiteral("。这些背景色与目标色重叠，建议补采不同位置或手动微调。")
                    : QString()));
    }
    crosshair::SetColorLabPreview(m_labPreviewToggle->isChecked()
        ? result.bands : std::vector<crosshair::ColorLabBand>{});
}

void CrosshairPage::applyLabProfile() {
    const auto result = crosshair::deriveColorLabBands(m_labTargets, m_labBackgrounds);
    if (result.bands.empty()) return;
    const int group = m_colors.size() + 1;
    for (size_t i = 0; i < result.bands.size(); ++i) {
        const auto& band = result.bands[i];
        ConfigManager::ColorProfile profile;
        profile.name = result.bands.size() == 1
            ? QStringLiteral("实验室 %1").arg(group)
            : QStringLiteral("实验室 %1-%2").arg(group).arg(i + 1);
        profile.enabled = true;
        profile.exactHsv = true;
        profile.hLow = band.h_low;
        profile.hHigh = band.h_high;
        profile.sMin = band.s_min;
        profile.sMax = band.s_max;
        profile.vMin = band.v_min;
        profile.vMax = band.v_max;
        m_colors.append(profile);
    }
    rebuildColorList();
    saveCrosshairColors();
}

void CrosshairPage::clearLabSamples() {
    m_labTargets.clear();
    m_labBackgrounds.clear();
    updateLabPreview();
}

void CrosshairPage::applyPickedColor(int h, int s, int v) {
    constexpr int kHueHalf = 5;
    constexpr int kSvMargin = 25;
    const int sLo = std::max(0, s - kSvMargin);
    const int sHi = std::min(255, s + kSvMargin);
    const int vLo = std::max(0, v - kSvMargin);
    const int vHi = std::min(255, v + kSvMargin);
    const QString base = QStringLiteral("取色 H%1 S%2 V%3").arg(h).arg(s).arg(v);

    const int lo = h - kHueHalf;
    const int hi = h + kHueHalf;

    if (lo < 0) {
        ConfigManager::ColorProfile c1;
        c1.name = base + QStringLiteral(" 低");
        c1.enabled = true;
        c1.exactHsv = true;
        c1.hLow = 0; c1.hHigh = hi;
        c1.sMin = sLo; c1.sMax = sHi;
        c1.vMin = vLo; c1.vMax = vHi;
        m_colors.append(c1);

        ConfigManager::ColorProfile c2;
        c2.name = base + QStringLiteral(" 高");
        c2.enabled = true;
        c2.exactHsv = true;
        c2.hLow = 180 + lo; c2.hHigh = 179;
        c2.sMin = sLo; c2.sMax = sHi;
        c2.vMin = vLo; c2.vMax = vHi;
        m_colors.append(c2);
    } else if (hi > 179) {
        ConfigManager::ColorProfile c1;
        c1.name = base + QStringLiteral(" 低");
        c1.enabled = true;
        c1.exactHsv = true;
        c1.hLow = 0; c1.hHigh = hi - 180;
        c1.sMin = sLo; c1.sMax = sHi;
        c1.vMin = vLo; c1.vMax = vHi;
        m_colors.append(c1);

        ConfigManager::ColorProfile c2;
        c2.name = base + QStringLiteral(" 高");
        c2.enabled = true;
        c2.exactHsv = true;
        c2.hLow = lo; c2.hHigh = 179;
        c2.sMin = sLo; c2.sMax = sHi;
        c2.vMin = vLo; c2.vMax = vHi;
        m_colors.append(c2);
    } else {
        ConfigManager::ColorProfile c;
        c.name = base;
        c.enabled = true;
        c.exactHsv = true;
        c.hLow = lo; c.hHigh = hi;
        c.sMin = sLo; c.sMax = sHi;
        c.vMin = vLo; c.vMax = vHi;
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
    m_labTargetBtn->setText(QStringLiteral("采目标色"));
    m_labBackgroundBtn->setText(QStringLiteral("采背景色"));
    m_pickRole = PickRole::Direct;
    m_pickColorBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:#302A1E; color:#D5B56B; border:1px solid rgba(213,181,107,0.25);"
        " border-radius:6px; padding:4px 14px; font-size:12px; font-weight:600;}"
        "QPushButton:hover{background:#3A3020; border-color:#D5B56B;}"));
}
