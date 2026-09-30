#include "MainWindow.h"

#include <QHBoxLayout>
#include <QCloseEvent>
#include <QInputDialog>
#include <QMessageBox>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QLineEdit>
#include <QPropertyAnimation>
#include <QProgressDialog>
#include <QShortcut>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <algorithm>
#include <mutex>

#include "Apotheosis.h"
#include "app_log.h"
#include "capture/capture.h"
#include "config/config.h"
#include "config/config_bridge.h"
#include "config/config_profiles.h"
#include "config/ConfigManager.h"
#include "detector/i_detector.h"
#include "runtime/inference_session.h"
#include "runtime/active_hotkey.h"
#include "runtime/latency_probe.h"
#include "runtime/sched_boost.h"
#include "tensorrt/trt_monitor.h"
#include "widgets/StatusBar.h"
#include "widgets/TopNavBar.h"
#include "widgets/SideNav.h"
#include "pages/OverviewPage.h"
#include "pages/SessionPage.h"
#include "pages/ModelToolsPage.h"
#include "pages/CapturePage.h"
#include "pages/AimSettingsPage.h"
#include "pages/TargetPage.h"
#include "pages/HardwarePage.h"
#include "pages/AiModelPage.h"

#include "pages/CrosshairPage.h"
#include "pages/AimpointRecoilPage.h"
#include "pages/AutoFlashPage.h"
#include "pages/MacroPage.h"
#include "pages/HeadBodyFusionPage.h"
#include "pages/LanTuningPage.h"
#include "pages/StatsPage.h"
#include "pages/LogPage.h"
#include "pages/DebugPage.h"
#include "pages/AutoCapturePage.h"

#include "capture/auto_capture.h"

namespace {

struct GroupDef {
    QString name;
    QStringList subs;
    QStringList icons;
};

const QVector<GroupDef>& navGroups() {
    static const QVector<GroupDef> kGroups = {
        {QString::fromUtf8(u8"概览"), {}, {}},
        {QString::fromUtf8(u8"会话"),
         {QString::fromUtf8(u8"推理启动"), QString::fromUtf8(u8"模型工具")},
         {QStringLiteral("player-play"), QStringLiteral("settings")}},
        {QString::fromUtf8(u8"配置"),
         {QString::fromUtf8(u8"画面采集"), QString::fromUtf8(u8"目标"), QString::fromUtf8(u8"硬件"),
          QString::fromUtf8(u8"AI 模型")},
         {QStringLiteral("device-desktop"), QStringLiteral("target"), QStringLiteral("plug"),
          QStringLiteral("cpu")}},
        {QString::fromUtf8(u8"控制"),
         {QString::fromUtf8(u8"瞄准设置"), QString::fromUtf8(u8"瞄点压枪"), QString::fromUtf8(u8"准星找色"), QString::fromUtf8(u8"自动爆闪"), QString::fromUtf8(u8"镭射找色"), QString::fromUtf8(u8"头身融合"), QString::fromUtf8(u8"宏编排"), QString::fromUtf8(u8"局域网调参")},
         {QStringLiteral("crosshair"), QStringLiteral("target"), QStringLiteral("color-swatch"), QStringLiteral("keyboard"), QStringLiteral("target"), QStringLiteral("target"), QStringLiteral("keyboard"), QStringLiteral("device-desktop")}},
        {QString::fromUtf8(u8"监控"),
         {QString::fromUtf8(u8"性能统计"), QString::fromUtf8(u8"日志"), QString::fromUtf8(u8"自动采集"),
          QString::fromUtf8(u8"调试")},
         {QStringLiteral("gauge"), QStringLiteral("terminal-2"), QStringLiteral("camera"),
          QStringLiteral("bug")}},
    };
    return kGroups;
}

}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("Apotheosis");
    resize(1080, 720);
    setMinimumSize(940, 600);

    auto* central = new QWidget(this);
    central->setObjectName("centralWidget");
    setCentralWidget(central);

    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_topNav = new TopNavBar(central);
    m_sideNav = new SideNav(central);
    m_pageStack = new QStackedWidget(central);
    m_statusBar = new StatusBar(central);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(m_sideNav);

    auto* pageShell = new QWidget(central);
    pageShell->setObjectName("pageShell");
    pageShell->setAttribute(Qt::WA_StyledBackground, true);
    auto* pageShellLayout = new QVBoxLayout(pageShell);
    pageShellLayout->setContentsMargins(0, 0, 0, 0);
    pageShellLayout->setSpacing(0);

    auto* contextHeader = new QWidget(pageShell);
    contextHeader->setObjectName("contextHeader");
    contextHeader->setAttribute(Qt::WA_StyledBackground, true);
    contextHeader->setFixedHeight(62);
    auto* contextLayout = new QVBoxLayout(contextHeader);
    contextLayout->setContentsMargins(20, 10, 20, 9);
    contextLayout->setSpacing(1);
    m_contextTitle = new QLabel(contextHeader);
    m_contextTitle->setObjectName("contextTitle");
    m_contextPath = new QLabel(contextHeader);
    m_contextPath->setObjectName("contextPath");
    contextLayout->addWidget(m_contextTitle);
    contextLayout->addWidget(m_contextPath);

    pageShellLayout->addWidget(contextHeader);
    pageShellLayout->addWidget(m_pageStack, 1);
    body->addWidget(pageShell, 1);

    layout->addWidget(m_topNav);
    layout->addLayout(body, 1);
    layout->addWidget(m_statusBar);

    setupPages();

    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, [this] {
        bool boosted = false;
        bool mmcss = false;
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            boosted = config.use_process_boost;
            mmcss = config.use_mmcss;
        }
        sched_boost::boostProcessPriority(boosted);
        capture_method_changed.store(true);
        if (m_overviewPage) m_overviewPage->setPerformanceMode(boosted && mmcss);
    });

    m_statusBar->setInferenceStatus(false);
    m_statusBar->setBackend("TRT");

    connect(m_topNav, &TopNavBar::primaryChanged, this, &MainWindow::onPrimaryChanged);
    connect(m_sideNav, &SideNav::currentChanged, this, &MainWindow::onSecondaryChanged);
    connect(m_topNav, &TopNavBar::saveClicked, this, &MainWindow::onSaveRequested);

    connect(m_topNav, &TopNavBar::profileSwitchRequested,
            this, &MainWindow::onProfileSwitchRequested);
    connect(m_topNav, &TopNavBar::profileSaveRequested,
            this, &MainWindow::onProfileSaveRequested);
    connect(m_topNav, &TopNavBar::profileSaveAsRequested,
            this, &MainWindow::onProfileSaveAsRequested);
    connect(m_topNav, &TopNavBar::profileRenameRequested,
            this, &MainWindow::onProfileRenameRequested);
    connect(m_topNav, &TopNavBar::profileDeleteRequested,
            this, &MainWindow::onProfileDeleteRequested);
    connect(m_topNav, &TopNavBar::profileOpenDirRequested,
            this, &MainWindow::onProfileOpenDirRequested);
    connect(m_topNav, &TopNavBar::profileRefreshRequested,
            this, &MainWindow::onProfileRefreshRequested);
    connect(&ConfigProfiles::instance(), &ConfigProfiles::profilesChanged,
            this, &MainWindow::refreshProfileControls);
    connect(&ConfigProfiles::instance(), &ConfigProfiles::operationFailed,
            this, [this](const QString& message) {
        QMessageBox::warning(this, QString::fromUtf8(u8"配置方案"), message);
    });
    refreshProfileControls();

    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, &MainWindow::onSaveRequested);

    m_topNav->setCurrentPrimary(0);
    onPrimaryChanged(0);

    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setInterval(200);
    connect(m_monitorTimer, &QTimer::timeout, this, &MainWindow::pollMonitorTelemetry);
    m_monitorTimer->start();
    pollMonitorTelemetry();
}

void MainWindow::setupPages() {
    int pageIndex = 0;
    QStringList primaryNames;

    for (const auto& g : navGroups()) {
        primaryNames << g.name;

        PageRange range;
        range.first = pageIndex;
        range.name = g.name;
        range.subs = g.subs;
        range.icons = g.icons;

        if (g.subs.isEmpty()) {
            m_overviewPage = new OverviewPage();
            connect(m_overviewPage, &OverviewPage::startStopRequested,
                    this, &MainWindow::onHeroToggleInference);
            connect(m_overviewPage, &OverviewPage::previewRequested,
                    this, [] { ConfigManager::instance().setShowWindow(true); });
            connect(m_overviewPage, &OverviewPage::performanceModeRequested,
                    this, [this](bool enabled) {
                if (!sched_boost::boostProcessPriority(enabled)) {
                    QMessageBox::warning(this, QString::fromUtf8(u8"性能模式"),
                        QString::fromUtf8(u8"无法调整进程优先级。"));
                    return;
                }
                {
                    std::lock_guard<std::recursive_mutex> lock(configMutex);
                    config.use_process_boost = enabled;
                    config.use_mmcss = enabled;
                    config.mmcss_task_name = "Games";
                }
                ConfigBridge::instance().markDirty();
                // The MF source reader's MMCSS class is fixed at creation.
                // Reopen capture so the mode switch covers its callback too.
                capture_method_changed.store(true);
                m_overviewPage->setPerformanceMode(enabled);
            });
            {
                std::lock_guard<std::recursive_mutex> lock(configMutex);
                m_overviewPage->setPerformanceMode(config.use_process_boost && config.use_mmcss);
            }
            m_pageStack->addWidget(m_overviewPage);
            range.count = 1;
            ++pageIndex;
        } else {
            range.count = g.subs.size();
            for (const auto& name : g.subs) {
                QWidget* page = createPage(name);
                m_pageStack->addWidget(page);
                ++pageIndex;
            }
        }

        m_tabPages.append(range);
    }

    m_topNav->setPrimaryItems(primaryNames);

    if (m_hotkeyPage && m_targetPage)
        m_hotkeyPage->setTargetPage(m_targetPage);
}

void MainWindow::selectPage(int primary, int secondary) {
    if (primary < 0 || primary >= m_tabPages.size())
        return;
    m_topNav->setCurrentPrimary(primary);
    onPrimaryChanged(primary);
    if (secondary > 0 && secondary < m_tabPages[primary].count) {
        m_sideNav->setCurrentIndex(secondary);
        onSecondaryChanged(secondary);
    }
}

void MainWindow::onPrimaryChanged(int index) {
    if (index < 0 || index >= m_tabPages.size())
        return;

    const PageRange& r = m_tabPages[index];
    if (r.subs.isEmpty()) {
        m_sideNav->hide();
    } else {
        m_sideNav->setItems(r.name, r.subs, r.icons);
        m_sideNav->show();
    }
    updateContextHeader(index, 0);
    switchPage(r.first);
}

void MainWindow::onSecondaryChanged(int index) {
    const int primaryIndex = m_topNav->currentPrimary();
    if (primaryIndex < 0 || primaryIndex >= m_tabPages.size())
        return;
    if (index < 0 || index >= m_tabPages[primaryIndex].count)
        return;

    updateContextHeader(primaryIndex, index);
    switchPage(m_tabPages[primaryIndex].first + index);
}

void MainWindow::switchPage(int index) {
    if (index < 0 || index >= m_pageStack->count())
        return;
    if (m_pageStack->currentIndex() == index)
        return;

    if (m_pageAnimation) {
        m_pageAnimation->stop();
        m_pageAnimation->deleteLater();
        m_pageAnimation = nullptr;
    }

    if (auto* previous = m_pageStack->currentWidget())
        previous->setGraphicsEffect(nullptr);

    m_pageStack->setCurrentIndex(index);
    QWidget* page = m_pageStack->currentWidget();
    const bool reduceMotion = qEnvironmentVariableIntValue("APOTHEOSIS_REDUCE_MOTION") != 0;
    if (!page || !isVisible() || reduceMotion)
        return;

    auto* effect = new QGraphicsOpacityEffect(page);
    effect->setOpacity(0.72);
    page->setGraphicsEffect(effect);

    auto* animation = new QPropertyAnimation(effect, "opacity", this);
    m_pageAnimation = animation;
    animation->setDuration(155);
    animation->setStartValue(0.72);
    animation->setEndValue(1.0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this, page, animation] {
        if (m_pageAnimation != animation)
            return;
        m_pageAnimation = nullptr;
        page->setGraphicsEffect(nullptr);
        animation->deleteLater();
    });
    animation->start();
}

void MainWindow::updateContextHeader(int primary, int secondary) {
    if (primary < 0 || primary >= m_tabPages.size())
        return;

    const PageRange& range = m_tabPages[primary];
    const QString pageName = range.subs.isEmpty() ? range.name : range.subs.value(secondary, range.name);
    m_contextTitle->setText(pageName);

    static const QStringList descriptions = {
        QString::fromUtf8(u8"运行状态与关键性能一览"),
        QString::fromUtf8(u8"推理会话与模型运行管理"),
        QString::fromUtf8(u8"采集、模型与硬件参数"),
        QString::fromUtf8(u8"瞄准逻辑与自动化控制"),
        QString::fromUtf8(u8"运行数据、日志与诊断"),
    };
    const QString description = descriptions.value(primary);
    if (range.subs.isEmpty()) {
        m_contextPath->setText(description);
    } else {
        m_contextPath->setText(QString::fromUtf8(u8"%1  /  %2 · %3")
                                   .arg(range.name, pageName, description));
    }
}

void MainWindow::onSaveRequested() {
    if (m_sessionOperation.valid()) return;
    ConfigBridge::instance().syncToRuntime();

    QString error;
    if (!ConfigProfiles::instance().saveCurrent(&error)) {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (!config.saveConfig()) {
            QMessageBox::warning(this, QString::fromUtf8(u8"保存设置"),
                                 error.isEmpty()
                                     ? QString::fromUtf8(u8"配置写入失败。")
                                     : error);
            return;
        }
    }
    m_topNav->showSaveFeedback();
    m_topNav->showProfileFeedback(QString::fromUtf8(u8"已保存"));
}

void MainWindow::refreshProfileControls() {
    auto& profiles = ConfigProfiles::instance();
    m_topNav->setProfiles(profiles.names(), profiles.activeName());
}

void MainWindow::onProfileSwitchRequested(const QString& name) {
    auto& profiles = ConfigProfiles::instance();
    if (name.isEmpty() || name == profiles.activeName()) {
        refreshProfileControls();
        return;
    }

    if (m_sessionRunning && !m_starting) {
        const auto answer = QMessageBox::question(
            this, QString::fromUtf8(u8"切换配置方案"),
            QString::fromUtf8(u8"推理正在运行。切换到「%1」后, 采集设备与模型等参数"
                              u8"需要重启推理会话才会完全生效。\n\n现在切换吗？")
                .arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            refreshProfileControls();
            return;
        }
    }

    QString error;
    if (!profiles.switchTo(name, &error)) {
        QMessageBox::warning(this, QString::fromUtf8(u8"切换配置方案"), error);
        refreshProfileControls();
        return;
    }
    m_topNav->showProfileFeedback(QString::fromUtf8(u8"已切换"));
}

void MainWindow::onProfileSaveRequested() {
    if (m_sessionOperation.valid()) return;
    QString error;
    if (!ConfigProfiles::instance().saveCurrent(&error)) {
        QMessageBox::warning(this, QString::fromUtf8(u8"保存配置方案"), error);
        return;
    }
    m_topNav->showProfileFeedback(QString::fromUtf8(u8"已保存"));
    m_topNav->showSaveFeedback();
}

void MainWindow::onProfileSaveAsRequested() {
    if (m_sessionOperation.valid()) return;
    auto& profiles = ConfigProfiles::instance();

    bool ok = false;
    const QString input = QInputDialog::getText(
        this, QString::fromUtf8(u8"另存为配置方案"),
        QString::fromUtf8(u8"方案名称（例如：游戏名 / 灵敏度档位）:"),
        QLineEdit::Normal, profiles.activeName(), &ok);
    if (!ok || input.trimmed().isEmpty())
        return;

    const QString name = ConfigProfiles::sanitizeName(input);
    if (name.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8(u8"另存为配置方案"),
                             QString::fromUtf8(u8"方案名不能为空, 且不能包含 "
                                               u8"\\ / : * ? \" < > | 等字符。"));
        return;
    }

    bool overwrite = false;
    if (profiles.exists(name)) {
        const auto answer = QMessageBox::question(
            this, QString::fromUtf8(u8"同名方案已存在"),
            QString::fromUtf8(u8"已存在配置方案「%1」, 覆盖它吗？").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        overwrite = true;
    }

    QString error;
    if (!profiles.saveAs(name, overwrite, &error)) {
        QMessageBox::warning(this, QString::fromUtf8(u8"另存为配置方案"), error);
        return;
    }
    m_topNav->showProfileFeedback(QString::fromUtf8(u8"已保存"));
}

void MainWindow::onProfileRenameRequested() {
    if (m_sessionOperation.valid()) return;
    auto& profiles = ConfigProfiles::instance();
    const QString active = profiles.activeName();
    if (active.isEmpty())
        return;

    bool ok = false;
    const QString input = QInputDialog::getText(
        this, QString::fromUtf8(u8"重命名配置方案"), QString::fromUtf8(u8"新的方案名称:"),
        QLineEdit::Normal, active, &ok);
    if (!ok)
        return;

    const QString name = ConfigProfiles::sanitizeName(input);
    if (name.isEmpty() || name == active) {
        if (name.isEmpty())
            QMessageBox::warning(this, QString::fromUtf8(u8"重命名配置方案"),
                                 QString::fromUtf8(u8"方案名无效。"));
        return;
    }

    QString error;
    if (!profiles.renameProfile(active, name, &error)) {
        QMessageBox::warning(this, QString::fromUtf8(u8"重命名配置方案"), error);
        return;
    }
    m_topNav->showProfileFeedback(QString::fromUtf8(u8"已重命名"));
}

void MainWindow::onProfileDeleteRequested() {
    if (m_sessionOperation.valid()) return;
    auto& profiles = ConfigProfiles::instance();
    const QString active = profiles.activeName();
    if (active.isEmpty())
        return;

    const auto answer = QMessageBox::warning(
        this, QString::fromUtf8(u8"删除配置方案"),
        QString::fromUtf8(u8"删除「%1」？该方案的配置文件与轨迹曲线会被永久删除, "
                          u8"无法撤销。\n\n删除后会自动切到另一个方案。")
            .arg(active),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    QString error;
    if (!profiles.remove(active, &error)) {
        QMessageBox::warning(this, QString::fromUtf8(u8"删除配置方案"), error);
        return;
    }
    m_topNav->showProfileFeedback(QString::fromUtf8(u8"已删除"));
}

void MainWindow::onProfileOpenDirRequested() {
    ConfigProfiles::instance().openDirectory();
}

void MainWindow::onProfileRefreshRequested() {
    ConfigProfiles::instance().refresh();
}

MainWindow::~MainWindow()
{
    if (m_sessionOperation.valid()) m_sessionOperation.wait();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_sessionOperation.valid())
    {
        m_closeRequested = true;
        event->ignore();
        return;
    }
    if (g_inference_session && (g_inference_session->running() || m_cleanupRequested))
    {
        m_closeRequested = true;
        beginSessionOperation(false);
        event->ignore();
        return;
    }
    event->accept();
}

void MainWindow::onHeroToggleInference()
{
    if (!g_inference_session || m_sessionOperation.valid()) return;
    beginSessionOperation(!g_inference_session->running());
}

void MainWindow::beginSessionOperation(bool start)
{
    if (!g_inference_session || m_sessionOperation.valid()) return;
    std::string backend, modelPath;
    if (start)
    {
        ConfigBridge::instance().syncToRuntime();
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        backend = config.backend;
        modelPath = "models/" + config.ai_model;
    }
    m_starting = start;
    m_cleanupRequested = start;
    m_pageStack->setEnabled(false);
    m_topNav->setEnabled(false);
    m_topNav->setSessionStatus(false, QString::fromUtf8(
        start ? u8"正在启动…" : u8"正在停止…"));
    auto* session = g_inference_session;
    try
    {
        m_sessionOperation = std::async(std::launch::async,
            [session, start, backend, modelPath]() -> std::string {
                if (start && !session->start(backend, modelPath)) return session->last_error();
                if (!start) session->stop();
                return {};
            });
    }
    catch (const std::exception& e)
    {
        m_pageStack->setEnabled(true);
        m_topNav->setEnabled(true);
        QMessageBox::critical(this, QString::fromUtf8(u8"会话操作失败"), QString::fromUtf8(e.what()));
    }
}

void MainWindow::pollSessionOperation()
{
    if (!m_sessionOperation.valid()) return;
    if (m_sessionOperation.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    std::string error;
    try { error = m_sessionOperation.get(); }
    catch (const std::exception& e) { error = e.what(); }
    catch (...) { error = "unknown session error"; }
    m_pageStack->setEnabled(true);
    m_topNav->setEnabled(true);
    if (!error.empty() && !m_closeRequested)
        QMessageBox::critical(this, QString::fromUtf8(u8"会话操作失败"), QString::fromUtf8(error.c_str()));
    if (m_closeRequested)
    {
        if (m_starting) beginSessionOperation(false);
        else close();
    }
}

void MainWindow::pollTrtBuildProgress()
{
    const TrtBuildSnapshot build = TrtBuildRead();
    if (build.generation == 0) return;

    if (build.generation != m_trtBuildGeneration)
    {
        m_trtBuildGeneration = build.generation;
        m_trtBuildFinishedMs = 0;
        if (!m_trtBuildDialog)
        {
            m_trtBuildDialog = new QProgressDialog(this);
            m_trtBuildDialog->setWindowTitle(QString::fromUtf8(u8"TensorRT 引擎构建"));
            m_trtBuildDialog->setCancelButton(nullptr);
            m_trtBuildDialog->setAutoClose(false);
            m_trtBuildDialog->setAutoReset(false);
            m_trtBuildDialog->setMinimumDuration(0);
            m_trtBuildDialog->setMinimumWidth(470);
        }
        if (build.active || build.stage == TrtBuildStage::Failed)
            m_trtBuildDialog->show();
    }
    if (!m_trtBuildDialog || !m_trtBuildDialog->isVisible()) return;

    const struct { TrtBuildStage stage; const char* name; } stages[] = {
        {TrtBuildStage::Reading, u8"读取模型"},
        {TrtBuildStage::Parsing, u8"解析 ONNX"},
        {TrtBuildStage::Configuring, u8"配置引擎"},
        {TrtBuildStage::Optimizing, u8"TensorRT 优化"},
        {TrtBuildStage::Loading, u8"加载引擎"},
        {TrtBuildStage::Saving, u8"保存缓存"},
    };
    int stageIndex = 0;
    QString stageName;
    for (int i = 0; i < 6; ++i)
        if (build.stage == stages[i].stage)
        {
            stageIndex = i + 1;
            stageName = QString::fromUtf8(stages[i].name);
            break;
        }

    QString label = QString::fromUtf8(u8"模型：%1\n").arg(QString::fromUtf8(build.model.c_str()));
    if (build.active)
    {
        label += QString::fromUtf8(u8"阶段 %1/6：%2\n").arg(stageIndex).arg(stageName);
        QStringList stageTrail;
        for (int i = 0; i < 6; ++i)
        {
            const QString marker = i + 1 < stageIndex ? QString::fromUtf8(u8"✓")
                : (i + 1 == stageIndex ? QString::fromUtf8(u8"●")
                                       : QString::fromUtf8(u8"○"));
            stageTrail << marker + QString::fromUtf8(stages[i].name);
        }
        label += stageTrail.mid(0, 3).join(QStringLiteral("  ")) + QLatin1Char('\n');
        label += stageTrail.mid(3, 3).join(QStringLiteral("  ")) + QLatin1Char('\n');
        if (!build.trtPhase.empty())
        {
            label += QString::fromUtf8(u8"TensorRT 内部阶段：%1").arg(
                QString::fromUtf8(build.trtPhase.c_str()));
            if (build.phaseMax > 0)
                label += QStringLiteral("  %1/%2").arg(build.phaseStep).arg(build.phaseMax);
            label += QLatin1Char('\n');
        }
        if (!build.detail.empty())
            label += QString::fromUtf8(build.detail.c_str()) + QLatin1Char('\n');
        const long long elapsed = std::max(0LL, TrtNowMs() - build.startedMs) / 1000;
        label += QString::fromUtf8(u8"已用 %1 秒").arg(elapsed);
        if (build.stage == TrtBuildStage::Optimizing && build.phaseMax > 0)
        {
            m_trtBuildDialog->setRange(0, build.phaseMax);
            m_trtBuildDialog->setValue(build.phaseStep);
        }
        else
        {
            m_trtBuildDialog->setRange(0, 0);
        }
    }
    else
    {
        const bool success = build.stage == TrtBuildStage::Complete;
        label += success ? QString::fromUtf8(u8"引擎构建完成")
                         : QString::fromUtf8(u8"引擎构建失败");
        if (!build.detail.empty())
            label += QString::fromUtf8(u8"：") + QString::fromUtf8(build.detail.c_str());
        m_trtBuildDialog->setRange(0, 1);
        m_trtBuildDialog->setValue(success ? 1 : 0);
        if (m_trtBuildFinishedMs == 0) m_trtBuildFinishedMs = TrtNowMs();
        if (TrtNowMs() - m_trtBuildFinishedMs >= (success ? 1500 : 4000))
            m_trtBuildDialog->hide();
    }
    m_trtBuildDialog->setLabelText(label);
}

QWidget* MainWindow::createPage(const QString& name) {
    if (name == QString::fromUtf8(u8"推理启动"))   return new SessionPage();
    if (name == QString::fromUtf8(u8"模型工具"))   return new ModelToolsPage();
    if (name == QString::fromUtf8(u8"画面采集"))   return new CapturePage();
    if (name == QString::fromUtf8(u8"目标"))       { m_targetPage = new TargetPage(); return m_targetPage; }
    if (name == QString::fromUtf8(u8"硬件"))       return new HardwarePage();
    if (name == QString::fromUtf8(u8"AI 模型"))    return new AiModelPage();
    if (name == QString::fromUtf8(u8"瞄准设置")) { m_hotkeyPage = new AimSettingsPage(); return m_hotkeyPage; }
    if (name == QString::fromUtf8(u8"瞄点压枪")) return new AimpointRecoilPage();
    if (name == QString::fromUtf8(u8"准星找色"))   return new CrosshairPage();
    if (name == QString::fromUtf8(u8"自动爆闪"))   return new AutoFlashPage();
    if (name == QString::fromUtf8(u8"镭射找色"))   return new CrosshairPage(nullptr, true);
    if (name == QString::fromUtf8(u8"宏编排"))     return new MacroPage();
    if (name == QString::fromUtf8(u8"头身融合"))   return new HeadBodyFusionPage();
    if (name == QString::fromUtf8(u8"局域网调参")) return new LanTuningPage();
    if (name == QString::fromUtf8(u8"性能统计"))   { m_statsPage = new StatsPage(); return m_statsPage; }
    if (name == QString::fromUtf8(u8"日志"))       { m_logPage   = new LogPage();   return m_logPage;   }
    if (name == QString::fromUtf8(u8"自动采集"))   { m_autoCapPage = new AutoCapturePage(); return m_autoCapPage; }
    if (name == QString::fromUtf8(u8"调试"))       { m_debugPage = new DebugPage(); return m_debugPage; }
    return new QWidget();
}

void MainWindow::pollMonitorTelemetry() {
    if (runtime::g_hotkey_activation_dirty.exchange(false))
        ConfigBridge::instance().markDirty();
    pollTrtBuildProgress();
    pollSessionOperation();
    if (g_inference_session && session_stop_requested.load()
        && m_cleanupRequested && !m_sessionOperation.valid())
    {
        beginSessionOperation(false);
    }
    const double fps = static_cast<double>(captureFps.load());
    const double sourceFps = static_cast<double>(captureSourceFps.load());

    const auto probe = runtime::latency::snapshot();
    const bool running = g_inference_session && g_inference_session->running();
    // Capture and inference samples are available before any aim hotkey is
    // active. frames_consumed only advances in the aim loop, so gating every
    // latency row on it hid valid measurements while detection was running.
    const auto stageMs = [&](runtime::latency::StageId stage) {
        return running && probe.stages[stage].n > 0
            ? probe.stages[stage].ema_ms : -1.0;
    };
    const double cap_ms = stageMs(runtime::latency::kCaptureWait);
    const double infer_chain_ms = stageMs(runtime::latency::kInference);
    const double pub2aim_ms = stageMs(runtime::latency::kPublishToAim);
    const double total_ms = stageMs(runtime::latency::kTotal);
    const double e2e_ms = stageMs(runtime::latency::kEndToEnd);
    const int deviceAgeUs = running ? probe.device_frame_age_us : -1;
    const double infer_ms = running ? probe.engine_inference_ms : -1.0;

    if (running && !m_sessionRunning)
        m_sessionStart = std::chrono::steady_clock::now();
    m_sessionRunning = running;

    int gpuMb = 0, cpuCores = 0;
    QString model;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        gpuMb = config.gpuMemoryReserveMB;
        cpuCores = config.cpuCoreReserveCount;
        model = QString::fromStdString(config.ai_model);
    }
    const QString backendDisp = QStringLiteral("TensorRT (CUDA)");

    m_statusBar->setInferenceStatus(running);
    m_statusBar->setFps(fps);
    if (!m_sessionOperation.valid())
        m_topNav->setSessionStatus(running, running ? QString::fromUtf8(u8"运行中") : QString::fromUtf8(u8"已停止"));

    if (m_overviewPage) {
        m_overviewPage->setFps(fps);
        m_overviewPage->setSourceFps(sourceFps);
        m_overviewPage->setInferenceLatency(infer_ms);
        m_overviewPage->setTotalLatency(e2e_ms);
        m_overviewPage->setCaptureChainDiagnostics(deviceAgeUs, cap_ms, infer_chain_ms,
                                                   pub2aim_ms, e2e_ms);
        int detectionCount = 0;
        {
            std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
            detectionCount = static_cast<int>(detectionBuffer.boxes.size());
        }
        m_overviewPage->setDetectionCount(detectionCount, -1);

        QString uptime;
        if (running) {
            const long s = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - m_sessionStart).count());
            uptime = QStringLiteral("%1:%2:%3")
                .arg(s / 3600, 2, 10, QChar('0'))
                .arg((s % 3600) / 60, 2, 10, QChar('0'))
                .arg(s % 60, 2, 10, QChar('0'));
        }
        m_overviewPage->setSessionState(
            running,
            model.isEmpty() ? QString::fromUtf8(u8"(未选择模型)") : model,
            backendDisp, uptime);
    }

    if (m_statsPage) {
        m_statsPage->setFps(fps);
        m_statsPage->setSourceFps(sourceFps);
        m_statsPage->setCaptureLatency(cap_ms);
        m_statsPage->setInferenceLatency(infer_ms);
        m_statsPage->setTotalLatency(total_ms);
        m_statsPage->setCaptureChainDiagnostics(deviceAgeUs, cap_ms, infer_chain_ms,
                                                pub2aim_ms, e2e_ms);
        const bool hasPipeline = running && probe.gpu_pipeline.n > 0;
        const bool hasBreakdown = hasPipeline && !probe.last_timing_was_graph;
        m_statsPage->setPipelineDiagnostics(
            hasPipeline ? probe.gpu_pipeline.ema_ms : -1.0,
            hasBreakdown ? probe.gpu_preprocess.ema_ms : -1.0,
            hasBreakdown ? probe.gpu_engine.ema_ms : -1.0,
            hasBreakdown ? probe.gpu_copy.ema_ms : -1.0,
            hasPipeline ? probe.cpu_postprocess.ema_ms : -1.0,
            hasPipeline ? probe.aim_tick.ema_ms : -1.0,
            hasPipeline ? probe.aim_tick.max_ms : -1.0,
            hasPipeline && probe.last_timing_was_graph);
        m_statsPage->setGpuMemory(QStringLiteral("%1 MB").arg(gpuMb));
        m_statsPage->setCpuCores(QString::number(cpuCores));
    }

    if (m_logPage) {
        const auto snap = AppLog::Snapshot();
        const int total = static_cast<int>(snap.size());
        if (m_logCursor > total) m_logCursor = 0;
        for (int i = m_logCursor; i < total; ++i)
            m_logPage->appendLog(QString::fromUtf8(snap[static_cast<size_t>(i)].c_str()));
        m_logCursor = total;
    }

    if (m_autoCapPage) {
        m_autoCapPage->setForceHeld(AutoCapture::g_force_held.load());
        m_autoCapPage->setSavedCounts(AutoCapture::g_saved_session.load(),
                                      AutoCapture::g_saved_total.load());
    }

}
