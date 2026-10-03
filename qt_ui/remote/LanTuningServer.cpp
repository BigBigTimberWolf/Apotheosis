#include "remote/LanTuningServer.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QMap>
#include <QVariant>
#include <cmath>
#include <mutex>
#include <type_traits>
#include "Apotheosis.h"
#include "config/config.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "runtime/config_snapshot.h"

namespace {
// One schema is used for rendering and validating updates. All edits are made
// to a candidate Config first; unknown keys and invalid values reject the batch.
struct Fields {
    QJsonArray schema;
    QJsonObject changes;
    QSet<QString> consumed;
    QString error;
    explicit Fields(QJsonObject patch = {}) : changes(std::move(patch)) {}

    template<class T> void number(const QString& key, const QString& label,
            const QString& section, T& value, double lo, double hi, double step) {
        constexpr bool integer = std::is_integral_v<T>;
        if (changes.contains(key)) {
            consumed.insert(key);
            const auto json = changes.value(key);
            const double n = json.toDouble();
            if (!json.isDouble() || !std::isfinite(n) || n < lo || n > hi ||
                (integer && std::floor(n) != n)) error = label + QStringLiteral("：数值超出范围或格式错误");
            else value = static_cast<T>(n);
        }
        schema.append(QJsonObject{{"key", key}, {"label", label}, {"section", section},
            {"type", "number"}, {"value", double(value)}, {"min", lo}, {"max", hi},
            {"step", step}, {"integer", integer}});
    }
    void boolean(const QString& key, const QString& label, const QString& section, bool& value) {
        if (changes.contains(key)) {
            consumed.insert(key);
            if (!changes.value(key).isBool()) error = label + QStringLiteral("：必须为开关值");
            else value = changes.value(key).toBool();
        }
        schema.append(QJsonObject{{"key", key}, {"label", label}, {"section", section},
            {"type", "boolean"}, {"value", value}});
    }
    void choice(const QString& key, const QString& label, const QString& section,
                std::string& value, const QStringList& options) {
        if (changes.contains(key)) {
            consumed.insert(key);
            if (!changes.value(key).isString() || !options.contains(changes.value(key).toString()))
                error = label + QStringLiteral("：选项已不存在，请刷新");
            else value = changes.value(key).toString().toStdString();
        }
        schema.append(QJsonObject{{"key", key}, {"label", label}, {"section", section},
            {"type", "choice"}, {"value", QString::fromStdString(value)},
            {"options", QJsonArray::fromStringList(options)}});
    }
    template<class T> void range(const QString& key, const QString& label, const QString& section,
                                 T& low, T& high, double lo, double hi, double step) {
        number(key + ".min", label + QStringLiteral(" · 下限"), section, low, lo, hi, step);
        number(key + ".max", label + QStringLiteral(" · 上限"), section, high, lo, hi, step);
        if ((changes.contains(key + ".min") || changes.contains(key + ".max")) && low > high)
            error = label + QStringLiteral("：下限不能大于上限");
    }
    QString validate() const {
        if (!error.isEmpty()) return error;
        if (consumed.size() != changes.size()) return QStringLiteral("包含未知或不可修改的参数，请刷新页面");
        return {};
    }
};

QStringList groups(const Config& c) {
    QStringList result;
    for (const auto& h : c.hotkeys) {
        const auto g = QString::fromStdString(h.group);
        if (!result.contains(g)) result.append(g);
    }
    return result;
}

void pidFields(Fields& f, control::RecoveredPidConfig& p, const QString& prefix, const QString& section) {
    f.number(prefix+"kpX", "比例 Kp · X", section, p.kpX, 0, 10, .01);
    f.number(prefix+"kpY", "比例 Kp · Y", section, p.kpY, 0, 10, .01);
    f.number(prefix+"kiX", "积分 Ki · X", section, p.kiX, 0, 10, .01);
    f.number(prefix+"kiY", "积分 Ki · Y", section, p.kiY, 0, 10, .01);
    f.number(prefix+"kdX", "微分 Kd · X", section, p.kdX, 0, 10, .01);
    f.number(prefix+"kdY", "微分 Kd · Y", section, p.kdY, 0, 10, .01);
    f.number(prefix+"ffX", "速度前馈 FF · X", section, p.feedforwardX, 0, 10, .0005);
    f.number(prefix+"ffY", "速度前馈 FF · Y", section, p.feedforwardY, 0, 10, .0005);
    f.number(prefix+"motionPxX", "FF 鼠标换算 · X（px/count）", section, p.motionPixelsPerCountX, .02, 20, .01);
    f.number(prefix+"motionPxY", "FF 鼠标换算 · Y（px/count）", section, p.motionPixelsPerCountY, .02, 20, .01);
    f.number(prefix+"motionDelayMs", "FF 响应延迟（ms，-1 使用旧窗口）", section, p.motionDelayMs, -1, 200, 1);
    f.number(prefix+"deadzoneX", "死区半径 · X", section, p.deadzoneX, 0, 200, .5);
    f.number(prefix+"deadzoneY", "死区半径 · Y", section, p.deadzoneY, 0, 200, .5);
    f.number(prefix+"hardDeadzoneX", "XY 轴死区 · X（内部不输出）", section, p.hardDeadzoneX, 0, 200, .5);
    f.number(prefix+"hardDeadzoneY", "XY 轴死区 · Y（内部不输出）", section, p.hardDeadzoneY, 0, 200, .5);
    f.number(prefix+"followX", "跟随补偿 · X", section, p.followX, 0, 50, .1);
    f.number(prefix+"followY", "跟随补偿 · Y", section, p.followY, 0, 50, .1);
    f.number(prefix+"maxPixel", "单帧限幅", section, p.smoothMaxPixel, 0, 1000, 1);
    f.number(prefix+"segment", "分段数", section, p.segment, 1, 10, .1);
    f.boolean(prefix+"segmentEnabled", "启用自定义分段（关闭时为 3）", section, p.segmentEnabled);
}

void hotkeyFields(Fields& f, HotkeyProfile& h) {
    const QString base = QStringLiteral("瞄准与 FOV");
    f.boolean("enabled", "启用瞄准控制器", base, h.ctl_enabled);
    f.boolean("crosshair", "使用准星找色", base, h.crosshair_detect_enabled);
    f.boolean("laser", "使用镭射找色", base, h.laser_detect_enabled);
    if (f.changes.value("crosshair") == QJsonValue(true)) h.laser_detect_enabled = false;
    if (f.changes.value("laser") == QJsonValue(true)) h.crosshair_detect_enabled = false;
    if (h.crosshair_detect_enabled && h.laser_detect_enabled) h.laser_detect_enabled = false;
    f.number("fovX", "FOV 水平直径", base, h.fovX, 1, 4096, 1);
    f.number("fovY", "FOV 垂直直径", base, h.fovY, 1, 4096, 1);
    f.number("aimDelayMs", "延迟瞄准（目标与热键同时生效后，ms）", base, h.aim_delay_ms, 0, 2000, 10);
    f.boolean("maskX", "瞄准目标时屏蔽 X 轴（真实鼠标输入）", base, h.mask_x);
    f.boolean("maskY", "瞄准目标时屏蔽 Y 轴（真实鼠标输入）", base, h.mask_y);
    f.boolean("unlockX", "解锁 X 轴（程序不横向瞄准）", base, h.unlock_x);
    f.boolean("unlockY", "解锁 Y 轴（程序不纵向瞄准）", base, h.unlock_y);
    f.number("unlockYDelayMs", "延迟解锁 Y（识别目标并按热键后，ms）", base,
             h.unlock_y_delay_ms, 0, 5000, 10);
    f.boolean("blockHotkey", "屏蔽瞄准热键", base, h.block_hotkey);
    f.boolean("dynamic.enabled", "启用动态 FOV", "动态 FOV", h.dynamic_fov_enabled);
    f.number("dynamic.size", "缩小后的 FOV 大小", "动态 FOV", h.dynamic_fov_size, 1, 4096, 1);
    f.number("dynamic.shrink", "缩小时间（ms）", "动态 FOV", h.dynamic_fov_shrink_ms, 0, 2000, 10);
    f.number("dynamic.expand", "放大时间（ms）", "动态 FOV", h.dynamic_fov_expand_ms, 0, 2000, 10);
    pidFields(f, h.recovered_pid, "main.", "主参数 · PID");
    bool scope = h.scope_ctl_enabled != 0;
    f.boolean("scope.enabled", "自动开镜时使用独立参数", "开镜参数 · PID", scope);
    if (f.changes.contains("scope.enabled")) h.scope_ctl_enabled = scope ? 1 : 0;
    pidFields(f, h.recovered_scope_pid, "scope.", "开镜参数 · PID");
    f.range("anchor.x", "瞄点 X（0 左 / 1 右）", "默认瞄点", h.ctl_x_offset, h.ctl_x_offset_max, 0, 1, .01);
    f.range("anchor.y", "瞄点 Y（0 底 / 1 顶）", "默认瞄点", h.ctl_y_offset, h.ctl_y_offset_max, 0, 1, .01);
    for (size_t i = 0; i < h.aim_classes.size(); ++i) {
        auto& a = h.aim_classes[i];
        const auto prefix = QString("class.%1.").arg(i);
        const auto title = QStringLiteral("类别 %1 · 优先级 %2").arg(a.class_id).arg(i+1);
        f.range(prefix+"x", "瞄点 X", title, a.x_offset, a.x_offset_max, 0, 1, .01);
        f.range(prefix+"y", "瞄点 Y", title, a.y_offset, a.y_offset_max, 0, 1, .01);
        f.number(prefix+"confidence", "最低置信度（0 跟随全局）", title, a.min_conf, 0, 1, .01);
    }
}

void globalFields(Fields& f, Config& c) {
    f.choice("activeGroup", "当前生效的热键分组", "运行分组", c.active_hotkey_group, groups(c));
    f.number("confidence", "置信度阈值", "识别参数", c.confidence_threshold, .01, 1, .01);
    f.number("nms", "NMS 阈值", "识别参数", c.nms_threshold, 0, 1, .01);
    f.boolean("small.enabled", "启用小目标增强", "识别参数", c.small_target_enabled);
    f.number("small.confidence", "小目标置信度", "识别参数", c.small_target_confidence, .01, 1, .01);
    f.number("small.area", "小目标面积比例", "识别参数", c.small_target_area_frac, .001, .1, .001);
    f.number("crosshair.width", "取样区域宽度（px）", "准星找色", c.crosshair_rect_w, 4, 256, 1);
    f.number("crosshair.height", "取样区域高度（px）", "准星找色", c.crosshair_rect_h, 4, 256, 1);
    f.number("crosshair.offsetY", "垂直偏移（正数向下，px）", "准星找色", c.crosshair_offset_y, -2048, 2048, 1);
    f.number("crosshair.pixels", "最小像素阈值", "准星找色", c.crosshair_min_pixel_count, 1, 200, 1);
    f.number("crosshair.close", "闭合滤波半径", "准星找色", c.crosshair_close_radius, 0, 7, 1);
    for (size_t i = 0; i < c.crosshair_colors.size(); ++i) {
        auto& p = c.crosshair_colors[i];
        const auto prefix = QString("color.%1.").arg(i);
        const auto title = QStringLiteral("颜色 %1 · %2").arg(i+1).arg(QString::fromStdString(p.name));
        f.boolean(prefix+"enabled", "启用颜色", title, p.enabled);
        f.range(prefix+"h", "色相 H", title, p.h_low, p.h_high, 0, 179, 1);
        f.range(prefix+"s", "饱和度 S", title, p.s_min, p.s_max, 0, 255, 1);
        f.range(prefix+"v", "亮度 V", title, p.v_min, p.v_max, 0, 255, 1);
    }
    f.number("laser.width", "取样区域宽度（px）", "镭射找色", c.laser_rect_w, 4, 4096, 1);
    f.number("laser.height", "取样区域高度（px）", "镭射找色", c.laser_rect_h, 4, 4096, 1);
    f.number("laser.centerX", "取样中心 X", "镭射找色", c.laser_center_x, 0, 8192, 1);
    f.number("laser.centerY", "取样中心 Y", "镭射找色", c.laser_center_y, 0, 8192, 1);
    f.number("laser.targetX", "瞄点区域中心 X", "镭射找色", c.laser_target_center_x, 0, 8192, 1);
    f.number("laser.targetY", "瞄点区域中心 Y", "镭射找色", c.laser_target_center_y, 0, 8192, 1);
    f.number("laser.targetW", "瞄点区域宽度", "镭射找色", c.laser_target_rect_w, 4, 4096, 1);
    f.number("laser.targetH", "瞄点区域高度", "镭射找色", c.laser_target_rect_h, 4, 4096, 1);
    f.number("laser.pixels", "最小像素阈值", "镭射找色", c.laser_min_pixel_count, 1, 10000, 1);
    f.number("laser.close", "闭合滤波半径", "镭射找色", c.laser_close_radius, 0, 9, 1);
    f.number("laser.elongation", "最小线条长宽比", "镭射找色", c.laser_min_elongation, 1, 30, .1);
    f.number("laser.smooth", "端点平滑强度", "镭射找色", c.laser_smooth, 0, 1, .05);
    for (size_t i = 0; i < c.laser_colors.size(); ++i) {
        auto& p = c.laser_colors[i];
        const auto prefix = QString("laserColor.%1.").arg(i);
        const auto title = QStringLiteral("镭射颜色 %1 · %2").arg(i+1).arg(QString::fromStdString(p.name));
        f.boolean(prefix+"enabled", "启用颜色", title, p.enabled);
        f.range(prefix+"h", "色相 H", title, p.h_low, p.h_high, 0, 179, 1);
        f.range(prefix+"s", "饱和度 S", title, p.s_min, p.s_max, 0, 255, 1);
        f.range(prefix+"v", "亮度 V", title, p.v_min, p.v_max, 0, 255, 1);
    }
}

QJsonObject snapshot(Config& c) {
    Fields global;
    globalFields(global, c);
    QJsonArray scopes;
    scopes.append(QJsonObject{{"id", "global"}, {"name", "识别与找色"},
        {"group", "全局"}, {"note", "调整全局识别阈值、准星与镭射取样区域和已有颜色范围。"}, {"fields", global.schema}});
    for (size_t i = 0; i < c.hotkeys.size(); ++i) {
        auto& h = c.hotkeys[i];
        Fields fields;
        hotkeyFields(fields, h);
        QStringList keys;
        for (const auto& k : h.keys) keys.append(QString::fromStdString(k));
        scopes.append(QJsonObject{{"id", QString("hotkey:%1").arg(i)},
            {"name", QString::fromStdString(h.name)}, {"group", QString::fromStdString(h.group)},
            {"note", QStringLiteral("触发键：%1。编辑某套参数不会自动切换游戏中正在使用的参数档。类别瞄点优先于默认瞄点；上下限相同时固定瞄点。").arg(keys.join(" + "))},
            {"fields", fields.schema}});
    }
    QJsonObject result{{"profile", QFileInfo(QString::fromStdString(c.configPath())).completeBaseName()},
        {"activeGroup", QString::fromStdString(c.active_hotkey_group)}, {"scopes", scopes}};
    const QByteArray hashInput = QByteArray::fromStdString(c.configPath()) + '\0' +
        QJsonDocument(result).toJson(QJsonDocument::Compact);
    result.insert("revision", QString::fromLatin1(QCryptographicHash::hash(hashInput, QCryptographicHash::Sha256).toHex()));
    return result;
}
QByteArray jsonError(const QString& error) {
    return QJsonDocument(QJsonObject{{"error", error}}).toJson(QJsonDocument::Compact);
}
}

LanTuningServer::LanTuningServer(QObject* parent) : QObject(parent), server_(new QTcpServer(this)) {
    connect(server_, &QTcpServer::newConnection, this, &LanTuningServer::acceptConnections);
    server_->setMaxPendingConnections(16);
}
LanTuningServer::~LanTuningServer() { stop(); }
bool LanTuningServer::running() const { return server_->isListening(); }
bool LanTuningServer::start(quint16 port, QString& error) {
    stop();
    if (port < 1024 || !server_->listen(QHostAddress::AnyIPv4, port)) {
        error = port < 1024 ? QStringLiteral("端口必须在 1024～65535 之间") : server_->errorString();
        return false;
    }
    for (int i = 0; i < 4; ++i)
        token_ += QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0');
    lastWrite_.invalidate();
    emit activity(QStringLiteral("已开启，等待浏览器连接"));
    return true;
}
void LanTuningServer::stop() {
    server_->close();
    const auto sockets = sockets_;
    for (auto* s : sockets) s->abort();
    token_.clear();
}
QStringList LanTuningServer::urls() const {
    QStringList result;
    if (!running()) return result;
    for (const auto& nic : QNetworkInterface::allInterfaces()) {
        if (!nic.flags().testFlag(QNetworkInterface::IsUp) ||
            !nic.flags().testFlag(QNetworkInterface::IsRunning) ||
            nic.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        for (const auto& entry : nic.addressEntries()) {
            const auto ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLoopback() ||
                ip.toString().startsWith("169.254.")) continue;
            result.append(QString("http://%1:%2/#%3").arg(ip.toString()).arg(server_->serverPort()).arg(QString::fromLatin1(token_)));
        }
    }
    result.removeDuplicates();
    result.append(QString("http://127.0.0.1:%1/#%2").arg(server_->serverPort()).arg(QString::fromLatin1(token_)));
    return result;
}
void LanTuningServer::acceptConnections() {
    while (server_->hasPendingConnections()) {
        auto* s = server_->nextPendingConnection();
        if (sockets_.size() >= 24) { s->abort(); s->deleteLater(); continue; }
        sockets_.insert(s);
        s->setReadBufferSize(80 * 1024);
        connect(s, &QTcpSocket::readyRead, this, [this, s] { readRequest(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            sockets_.remove(s); s->deleteLater();
        });
        QTimer::singleShot(5000, s, [s] { s->abort(); });
        if (s->bytesAvailable()) readRequest(s);
    }
}
void LanTuningServer::reply(QTcpSocket* s, int status, const QByteArray& body, const QByteArray& type) {
    if (s->property("responded").toBool()) return;
    s->setProperty("responded", true);
    const QByteArray reason = status == 200 ? "OK" : "Request Failed";
    s->write("HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\nContent-Type: " + type +
        "\r\nContent-Length: " + QByteArray::number(body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff"
        "\r\nReferrer-Policy: no-referrer\r\nX-Frame-Options: DENY"
        "\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n\r\n" + body);
    s->disconnectFromHost();
}
void LanTuningServer::readRequest(QTcpSocket* s) {
    if (s->property("responded").toBool()) return;
    QByteArray data = s->property("request").toByteArray() + s->readAll();
    if (data.size() > 72*1024) { reply(s, 413, jsonError("请求过大")); return; }
    s->setProperty("request", data);
    const auto end = data.indexOf("\r\n\r\n");
    if (end < 0) {
        if (data.size() > 8192) reply(s, 431, jsonError("请求头过大"));
        return;
    }
    if (end > 8192) { reply(s, 431, jsonError("请求头过大")); return; }
    const auto lines = data.left(end).split('\n');
    const auto first = lines.front().trimmed().split(' ');
    if (first.size() != 3 || first[2] != "HTTP/1.1") { reply(s, 400, jsonError("无效请求")); return; }
    QMap<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const auto colon = lines[i].indexOf(':');
        const auto name = lines[i].left(colon).trimmed().toLower();
        if (colon <= 0 || headers.contains(name)) { reply(s, 400, jsonError("无效请求头")); return; }
        headers.insert(name, lines[i].mid(colon+1).trimmed());
    }
    if (!headers.contains("host") || headers.contains("transfer-encoding")) {
        reply(s, 400, jsonError("不支持的请求格式")); return;
    }
    bool lengthOk = true;
    const qlonglong length = headers.contains("content-length") ? headers["content-length"].toLongLong(&lengthOk) : 0;
    if (!lengthOk || length < 0 || length > 64*1024) { reply(s, 413, jsonError("请求长度无效")); return; }
    if (data.size() < end+4+length) return;
    if (data.size() != end+4+length) { reply(s, 400, jsonError("不支持批量请求")); return; }
    const auto method = first[0], path = first[1];
    if (method == "GET" && (path == "/" || path == "/index.html" || path == "/app.js" || path == "/style.css")) {
        QFile file(":/lan" + QString::fromLatin1(path == "/" ? QByteArray("/index.html") : path));
        if (!file.open(QIODevice::ReadOnly)) { reply(s, 500, jsonError("网页资源不可用")); return; }
        reply(s, 200, file.readAll(), path.endsWith(".js") ? "text/javascript; charset=utf-8" :
              path.endsWith(".css") ? "text/css; charset=utf-8" : "text/html; charset=utf-8");
        return;
    }
    if (headers.value("authorization") != "Bearer " + token_ || token_.isEmpty()) {
        reply(s, 401, jsonError("访问凭据无效，请从本机重新复制完整地址")); return;
    }
    if (headers.contains("origin") && headers.value("origin") != "http://" + headers.value("host")) {
        reply(s, 403, jsonError("不允许跨站请求")); return;
    }
    if (method == "GET" && path == "/api/state") {
        reply(s, 200, QJsonDocument(state()).toJson(QJsonDocument::Compact)); return;
    }
    if (method == "POST" && path == "/api/apply") {
        if (headers.value("content-type").split(';').front().trimmed() != "application/json") {
            reply(s, 415, jsonError("需要 JSON 请求")); return;
        }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(data.mid(end+4, length), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            reply(s, 400, jsonError("JSON 格式错误")); return;
        }
        apply(s, doc.object()); return;
    }
    reply(s, 404, jsonError("地址不存在"));
}
QJsonObject LanTuningServer::state() {
    std::lock_guard<std::recursive_mutex> lock(configMutex);
    return snapshot(config);
}
void LanTuningServer::apply(QTcpSocket* s, const QJsonObject& request) {
    if (lastWrite_.isValid() && lastWrite_.elapsed() < 150) {
        reply(s, 429, jsonError("操作过快，请稍后重试")); return;
    }
    lastWrite_.start();
    if (!request.value("scope").isString() || !request.value("changes").isObject() ||
        !request.value("revision").isString() || request.value("changes").toObject().isEmpty()) {
        reply(s, 400, jsonError("没有有效的参数修改")); return;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        if (request.value("revision") != snapshot(config).value("revision")) {
            reply(s, 409, jsonError("本机或另一浏览器已修改配置，请先重新读取，再应用修改")); return;
        }
        Config candidate = config;
        Fields fields(request.value("changes").toObject());
        const auto scope = request.value("scope").toString();
        if (scope == "global") globalFields(fields, candidate);
        else {
            bool ok = false;
            const int i = scope.mid(7).toInt(&ok);
            if (!scope.startsWith("hotkey:") || !ok || i < 0 || i >= int(candidate.hotkeys.size())) {
                reply(s, 404, jsonError("热键配置已不存在")); return;
            }
            hotkeyFields(fields, candidate.hotkeys[i]);
        }
        const auto error = fields.validate();
        if (!error.isEmpty()) { reply(s, 400, jsonError(error)); return; }
        // Same serialization as desktop saves; report write failures rather
        // than claiming an unsaved update succeeded. No hardware reconfigure.
        if (!candidate.saveConfig()) { reply(s, 500, jsonError("配置文件保存失败，参数未应用")); return; }
        config = std::move(candidate);
        runtime_config::publish();
    }
    ConfigBridge::instance().syncFromRuntime();
    ConfigManager::instance().notifyRuntimeReloaded();
    emit activity(QStringLiteral("%1 已应用并保存参数").arg(s->peerAddress().toString()));
    reply(s, 200, QJsonDocument(state()).toJson(QJsonDocument::Compact));
}
