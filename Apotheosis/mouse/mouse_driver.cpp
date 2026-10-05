#include "mouse_driver.h"

#include <algorithm>
#include <memory>
#include <sstream>

#include "Makcu.h"
#include "MakcuNew.h"
#include "kmboxNetConnection.h"
#include "dhzbox_driver.h"
#include "ferrum_driver.h"
#include "cat_driver.h"
#include "cpbox_driver.h"
#include "windows_driver.h"

namespace mouse_driver
{

const char* const kBackendMakcu     = "MAKCU";
const char* const kBackendMakcuNew  = "MAKCUNEW";
const char* const kBackendKmboxNet  = "KMBOXNET";
const char* const kBackendFerrum = "FERRUM";
const char* const kBackendDhzboxMini = "DHZBOX_MINI";
const char* const kBackendCpbox = "CPBOX";

std::string describeCapabilities(uint32_t caps)
{
    static const uint32_t order[] = {
        kCapMove, kCapButtonLeft, kCapButtonRight, kCapButtonMiddle,
        kCapButtonSide, kCapWheel, kCapKeyboard, kCapKeyboardMask, kCapPhysicalRead
    };
    std::string out;
    for (uint32_t c : order)
    {
        if ((caps & c) == 0) continue;
        if (!out.empty()) out += ' ';
        out += capabilityName(c);
    }
    if (out.empty()) out = u8"无";
    return out;
}

std::vector<std::string> backendNames()
{
    return { kBackendMakcu, kBackendMakcuNew, kBackendKmboxNet,
             kBackendFerrum, kBackendDhzboxMini, "WINDOWS", "CAT", kBackendCpbox };
}

std::string describeStatus(const std::string& backend, bool open, const std::string& detail)
{
    std::string out = u8"鼠标后端 ";
    out += backend.empty() ? std::string(u8"(未选择)") : backend;
    if (!detail.empty())
    {
        out += " (";
        out += detail;
        out += ")";
    }
    out += open ? u8" 已连接" : u8" 不可用";
    if (!open && !detail.empty())
    {
        out += u8": ";
        out += detail;
    }
    return out;
}

namespace
{

std::string errWith(const char* what, const std::string& detail)
{
    std::string s = what;
    if (!detail.empty())
    {
        s += u8" (";
        s += detail;
        s += ')';
    }
    return s;
}

}

WrappedMakcuDriver::WrappedMakcuDriver(MakcuConnection* conn) : conn_(conn) {}

const char* WrappedMakcuDriver::name() const { return kBackendMakcu; }

uint32_t WrappedMakcuDriver::capabilities() const
{
    return kCapMove | kCapButtonLeft | kCapButtonRight | kCapButtonMiddle |
           kCapButtonSide | kCapWheel | kCapPhysicalRead;
}

bool WrappedMakcuDriver::isOpen() const { return conn_ != nullptr && conn_->isOpen(); }

std::string WrappedMakcuDriver::lastError() const
{
    return u8"[MAKCU] 串口未打开或设备未响应";
}

bool WrappedMakcuDriver::move(int dx, int dy)
{
    if (!isOpen()) return false;
    conn_->move(dx, dy);
    return conn_->isOpen();
}

bool WrappedMakcuDriver::leftDown()   { if (!isOpen()) return false; conn_->press(1);   return true; }
bool WrappedMakcuDriver::leftUp()     { if (!isOpen()) return false; conn_->release(1); return true; }
bool WrappedMakcuDriver::rightDown()  { if (!isOpen()) return false; conn_->press(2);   return true; }
bool WrappedMakcuDriver::rightUp()    { if (!isOpen()) return false; conn_->release(2); return true; }
bool WrappedMakcuDriver::middleDown() { if (!isOpen()) return false; conn_->press(3);   return true; }
bool WrappedMakcuDriver::middleUp()   { if (!isOpen()) return false; conn_->release(3); return true; }

bool WrappedMakcuDriver::wheel(int delta)
{
    if (!isOpen()) return false;
    conn_->wheel(delta);
    return true;
}

bool WrappedMakcuDriver::tapKey(int, int, int) { return false; }

int WrappedMakcuDriver::physicalButtonPressed(int button) const
{
    if (!isOpen()) return -1;
    switch (button)
    {
    case 1: return conn_->shooting_active ? 1 : 0;
    case 2: return conn_->zooming_active  ? 1 : 0;
    case 3: return conn_->middle_active   ? 1 : 0;
    case 4: return conn_->side1_active    ? 1 : 0;
    case 5: return conn_->side2_active    ? 1 : 0;
    default: return -1;
    }
}

WrappedMakcuNewDriver::WrappedMakcuNewDriver(MakcuNewConnection* connMouse,
                                             MakcuNewConnection* connKbd)
    : conn_mouse_(connMouse), conn_kbd_(connKbd) {}

const char* WrappedMakcuNewDriver::name() const { return kBackendMakcuNew; }

uint32_t WrappedMakcuNewDriver::capabilities() const
{
    // 鼠标能力恒有; 键盘能力【只在这台真的接了键盘硬件时】才声明。
    //
    // 上层用独立的键盘与真实输入屏蔽能力位判断可用功能。
    // 若这里无脑声明键盘能力, 但实际没有键盘硬件, 上层会一路走到 tapKey(),
    // 而 tapKey 必须失败 —— 表现为"自动急停配了却没反应", 且无法从能力位看出原因。
    // 如实上报后, 上层能直接判断出不可用。
    uint32_t caps = kCapMove | kCapButtonLeft | kCapButtonRight | kCapButtonMiddle |
                    kCapButtonSide | kCapWheel | kCapPhysicalRead;
    if (keyboardConnection() != nullptr)
        caps |= kCapKeyboard | kCapKeyboardMask;
    return caps;
}

bool WrappedMakcuNewDriver::isOpen() const { return conn_mouse_ != nullptr && conn_mouse_->isOpen(); }

std::string WrappedMakcuNewDriver::lastError() const
{
    return u8"[MAKCUNEW] 串口未打开或会话探活失败(固件上电必为 115200, 协商失败会自动退回)";
}

bool WrappedMakcuNewDriver::move(int dx, int dy)
{
    if (!isOpen()) return false;
    return conn_mouse_->move(dx, dy);
}

bool WrappedMakcuNewDriver::leftDown()   { return isOpen() && conn_mouse_->press(1); }
bool WrappedMakcuNewDriver::leftUp()     { return isOpen() && conn_mouse_->release(1); }
bool WrappedMakcuNewDriver::rightDown()  { return isOpen() && conn_mouse_->press(2); }
bool WrappedMakcuNewDriver::rightUp()    { return isOpen() && conn_mouse_->release(2); }
bool WrappedMakcuNewDriver::middleDown() { return isOpen() && conn_mouse_->press(3); }
bool WrappedMakcuNewDriver::middleUp()   { return isOpen() && conn_mouse_->release(3); }

bool WrappedMakcuNewDriver::wheel(int delta)
{
    if (!isOpen()) return false;
    conn_mouse_->wheel(delta);
    return true;
}

bool WrappedMakcuNewDriver::tapKey(int hidKey, int holdMs, int mod)
{
    if (!isOpen()) return false;
    // 键盘动作走【键盘那台硬件】: 只插键盘的硬件才有意义把键盘事件送给它,
    // 送错硬件会被它自己的 C 口真实设备状态覆盖/冲突。
    MakcuNewConnection* kb = keyboardConnection();
    if (!kb || !kb->isOpen()) return false;
    return kb->tapKey(hidKey, holdMs, mod);
}

// 屏蔽真实键盘输入 durationMs 毫秒 —— 从【键盘那台硬件】发出。
//
// 自动急停的核心动作: 触发时把玩家按住的方向键冻住, 角色凭游戏自身惯性/
// 停止行为停下, 而不是注入反向键去"刹车"。因此这里只发 km.mask, 不注入任何键。
//
// <=0 走 maskOff(解除), 与固件 km.maskoff 语义一致。
bool WrappedMakcuNewDriver::maskRealKeyboard(int durationMs)
{
    if (!isOpen()) return false;
    MakcuNewConnection* kb = keyboardConnection();
    if (!kb || !kb->isOpen()) return false;
    return (durationMs > 0) ? kb->mask(durationMs) : kb->maskOff();
}

int WrappedMakcuNewDriver::physicalButtonPressed(int button) const
{
    if (!isOpen()) return -1;
    // 物理按键读数只对鼠标那台有意义(鼠标 C 口插的是鼠标)。
    const bool down = conn_mouse_->physicalButtonPressed(button);
    return down ? 1 : 0;
}

void WrappedMakcuNewDriver::cancelMove()
{
    if (isOpen()) conn_mouse_->cancelMove();
}

bool WrappedMakcuNewDriver::directSend() const { return true; }

// =============================================================================
// WrappedHybridDriver: 鼠标(官方 SDK / MakcuConnection) + 键盘(MakcuNewConnection)
// =============================================================================

WrappedHybridDriver::WrappedHybridDriver(MakcuConnection* mouseConn, MakcuNewConnection* kbdConn)
    : mouseConn_(mouseConn), kbdConn_(kbdConn) {}

const char* WrappedHybridDriver::name() const { return kBackendMakcu; }

uint32_t WrappedHybridDriver::capabilities() const
{
    uint32_t caps = kCapMove | kCapButtonLeft | kCapButtonRight | kCapButtonMiddle |
                    kCapButtonSide | kCapWheel | kCapPhysicalRead;
    // 键盘能力只在键盘那台真实存在时声明 (跟 WrappedMakcuNewDriver 的判断一致)。
    if (kbdConn_ != nullptr) caps |= kCapKeyboard | kCapKeyboardMask;
    return caps;
}

bool WrappedHybridDriver::isOpen() const
{
    return mouseConn_ != nullptr && mouseConn_->isOpen();
}

std::string WrappedHybridDriver::lastError() const
{
    return u8"[Hybrid] 鼠标(MAKCU/ASCII)串口未打开或设备未响应";
}

bool WrappedHybridDriver::move(int dx, int dy)
{
    if (!isOpen()) return false;
    mouseConn_->move(dx, dy);
    return mouseConn_->isOpen();
}

bool WrappedHybridDriver::leftDown()   { if (!isOpen()) return false; mouseConn_->press(1);   return true; }
bool WrappedHybridDriver::leftUp()     { if (!isOpen()) return false; mouseConn_->release(1); return true; }
bool WrappedHybridDriver::rightDown()  { if (!isOpen()) return false; mouseConn_->press(2);   return true; }
bool WrappedHybridDriver::rightUp()    { if (!isOpen()) return false; mouseConn_->release(2); return true; }
bool WrappedHybridDriver::middleDown() { if (!isOpen()) return false; mouseConn_->press(3);   return true; }
bool WrappedHybridDriver::middleUp()   { if (!isOpen()) return false; mouseConn_->release(3); return true; }

bool WrappedHybridDriver::wheel(int delta)
{
    if (!isOpen()) return false;
    mouseConn_->wheel(delta);
    return true;
}

bool WrappedHybridDriver::tapKey(int hidKey, int holdMs, int mod)
{
    // 键盘 tap 必须走键盘那台硬件 —— 送错设备就是空操作。
    if (kbdConn_ == nullptr || !kbdConn_->isOpen()) return false;
    return kbdConn_->tapKey(hidKey, holdMs, mod);
}

bool WrappedHybridDriver::maskRealKeyboard(int durationMs)
{
    if (kbdConn_ == nullptr || !kbdConn_->isOpen()) return false;
    return (durationMs > 0) ? kbdConn_->mask(durationMs) : kbdConn_->maskOff();
}

int WrappedHybridDriver::physicalButtonPressed(int button) const
{
    if (!isOpen()) return -1;
    // 鼠标物理按键回读来自官方 SDK 的按键回调 —— shooting_active 等成员。
    switch (button)
    {
    case 1: return mouseConn_->shooting_active ? 1 : 0;
    case 2: return mouseConn_->zooming_active  ? 1 : 0;
    case 3: return mouseConn_->middle_active   ? 1 : 0;
    case 4: return mouseConn_->side1_active    ? 1 : 0;
    case 5: return mouseConn_->side2_active    ? 1 : 0;
    default: return -1;
    }
}

WrappedKmboxNetDriver::WrappedKmboxNetDriver(KmboxNetConnection* conn) : conn_(conn) {}
WrappedKmboxNetDriver::~WrappedKmboxNetDriver() { maskRealKeyboard(0); }

const char* WrappedKmboxNetDriver::name() const { return kBackendKmboxNet; }

uint32_t WrappedKmboxNetDriver::capabilities() const
{
    return kCapMove | kCapButtonLeft | kCapButtonRight | kCapButtonMiddle |
           kCapButtonSide | kCapWheel | kCapKeyboard | kCapKeyboardMask |
           kCapPhysicalRead;
}

bool WrappedKmboxNetDriver::isOpen() const { return conn_ != nullptr && conn_->isOpen(); }

std::string WrappedKmboxNetDriver::lastError() const
{
    return u8"[KMBOXNET] UDP 连接失败(核对盒子屏幕上的 IP/端口/UUID)";
}

bool WrappedKmboxNetDriver::move(int dx, int dy)
{
    if (!isOpen()) return false;
    conn_->move(dx, dy);
    return true;
}

bool WrappedKmboxNetDriver::leftDown()   { if (!isOpen()) return false; conn_->leftDown();   return true; }
bool WrappedKmboxNetDriver::leftUp()     { if (!isOpen()) return false; conn_->leftUp();     return true; }
bool WrappedKmboxNetDriver::rightDown()  { if (!isOpen()) return false; conn_->rightDown();  return true; }
bool WrappedKmboxNetDriver::rightUp()    { if (!isOpen()) return false; conn_->rightUp();    return true; }
bool WrappedKmboxNetDriver::middleDown() { if (!isOpen()) return false; conn_->middleDown(); return true; }
bool WrappedKmboxNetDriver::middleUp()   { if (!isOpen()) return false; conn_->middleUp();   return true; }

bool WrappedKmboxNetDriver::wheel(int delta)
{
    if (!isOpen()) return false;
    conn_->wheel(delta);
    return true;
}

bool WrappedKmboxNetDriver::tapKey(int hidKey, int holdMs, int)
{
    if (!isOpen() || hidKey < 4 || hidKey > 231) return false;
    const bool down = conn_->keyDown(hidKey);
    if (holdMs > 0) Sleep(static_cast<DWORD>(holdMs));
    const bool up = conn_->keyUp(hidKey);
    return down && up;
}

bool WrappedKmboxNetDriver::maskRealKeyboard(int durationMs)
{
    // KMBoxNet masks one physical HID key per command. For auto-stop, only
    // movement keys need to be held back; other keyboard input stays usable.
    constexpr std::array<short, 4> movementKeys{4, 7, 22, 26}; // A D S W
    if (!conn_ || !conn_->isOpen()) return false;
    if (durationMs > 0 && movementMaskUncertain_ && !maskRealKeyboard(0))
        return false;
    bool okay = true;
    for (size_t i = 0; i < movementKeys.size(); ++i)
    {
        if (durationMs > 0 && !movementKeysMasked_[i])
        {
            if (conn_->maskKeyboard(movementKeys[i])) movementKeysMasked_[i] = true;
            else
            {
                // A timed-out UDP reply does not prove the box missed the
                // command. Treat this key as possibly masked and undo it.
                movementKeysMasked_[i] = true;
                movementMaskUncertain_ = true;
                okay = false;
            }
        }
        else if (durationMs <= 0 && movementKeysMasked_[i])
        {
            if (conn_->unmaskKeyboard(movementKeys[i])) movementKeysMasked_[i] = false;
            else { movementMaskUncertain_ = true; okay = false; }
        }
    }
    if (!okay && durationMs > 0)
        maskRealKeyboard(0);
    if (durationMs <= 0 && okay)
        movementMaskUncertain_ = false;
    return okay;
}

int WrappedKmboxNetDriver::physicalButtonPressed(int button) const
{
    if (!isOpen()) return -1;
    switch (button)
    {
    case 1: return conn_->monitorMouseLeft();
    case 2: return conn_->monitorMouseRight();
    case 3: return conn_->monitorMouseMiddle();
    case 4: return conn_->monitorMouseSide1();
    case 5: return conn_->monitorMouseSide2();
    default: return -1;
    }
}

bool WrappedKmboxNetDriver::directSend() const { return true; }

OpenResult open(const std::string& backend,
                const std::string& makcuPort, unsigned int makcuBaud,
                const std::string& makcuNewPort, unsigned int makcuNewBaud,
                const std::string& kmboxNetIp, const std::string& kmboxNetPort,
                const std::string& kmboxNetUuid,
                const std::string& makcuNewPortKbd, unsigned int makcuNewBaudKbd,
                const std::string& ferrumPort, unsigned int ferrumBaud,
                const std::string& dhzboxIp, unsigned short dhzboxPort, int dhzboxKey,
                const std::string& catIp, unsigned short catPort,
                const std::string& catUuid, unsigned short catMonitorPort,
                const std::string& cpboxPort)
{
    OpenResult result;
    if (backend == "WINDOWS") {
        auto driver = std::make_unique<WindowsDriver>();
        if (!driver->isOpen()) { result.error=driver->lastError(); return result; }
        result.driver=driver.release(); return result;
    }

    if (backend == kBackendMakcu)
    {
        auto conn = std::make_unique<MakcuConnection>(makcuPort, makcuBaud);
        if (!conn->isOpen())
        {
            result.error = errWith(u8"[MAKCU] 串口打不开或设备无响应",
                                   makcuPort + "@" + std::to_string(makcuBaud));
            return result;
        }
        result.driver = new OwningDriver<WrappedMakcuDriver>(std::move(conn));
        return result;
    }

    if (backend == "CAT") {
        auto driver=std::make_unique<CatDriver>(catIp,catPort,catUuid,catMonitorPort);
        if(!driver->isOpen()) { result.error=driver->lastError(); return result; }
        result.driver=driver.release(); return result;
    }
    if (backend == kBackendCpbox) {
        auto driver = std::make_unique<CpboxDriver>(cpboxPort);
        if (!driver->isOpen()) { result.error = driver->lastError(); return result; }
        result.driver = driver.release(); return result;
    }
    if (backend == kBackendFerrum)
    {
        auto conn = std::make_unique<FerrumDriver>(ferrumPort, ferrumBaud);
        if (!conn->isOpen()) {
            result.error = errWith(u8"[FERRUM] 串口打不开或设备无响应", conn->lastError());
            return result;
        }
        result.driver = conn.release();
        return result;
    }

    if (backend == kBackendDhzboxMini)
    {
        auto conn = std::make_unique<DhzboxMiniDriver>(dhzboxIp, dhzboxPort, dhzboxKey);
        if (!conn->isOpen()) { result.error = conn->lastError(); return result; }
        result.driver = conn.release();
        return result;
    }

    if (backend == kBackendMakcuNew)
    {
        auto conn = std::make_unique<MakcuNewConnection>(makcuNewPort, makcuNewBaud);
        if (!conn->isOpen())
        {
            result.error = errWith(u8"[MAKCUNEW] 会话未建立(固件上电必为 115200, 协商失败自动退回)",
                                   makcuNewPort + "@" + std::to_string(makcuNewBaud));
            return result;
        }
        // 第二台(键盘)。端口为空或与第一台相同 => 不开, 键盘动作回落到第一台。
        // 打开失败不阻断启动: 鼠标那台仍可用, 只是键盘动作会落到鼠标硬件上
        // (真实键盘并不在那台上, 表现为键盘失效), 因此这里不报致命错误。
        std::unique_ptr<MakcuNewConnection> connKbd;
        if (!makcuNewPortKbd.empty() && makcuNewPortKbd != makcuNewPort)
        {
            connKbd = std::make_unique<MakcuNewConnection>(makcuNewPortKbd, makcuNewBaudKbd);
            if (!connKbd->isOpen())
                connKbd.reset();
        }

        result.driver = new OwningDriver<WrappedMakcuNewDriver>(
            std::move(conn), std::move(connKbd));
        return result;
    }

    if (backend == kBackendKmboxNet)
    {
        if (kmboxNetIp.empty())
        {
            result.error = u8"[KMBOXNET] 未填写盒子 IP (显示在盒子屏幕上, 例如 192.168.2.88)";
            return result;
        }
        auto conn = std::make_unique<KmboxNetConnection>(kmboxNetIp, kmboxNetPort, kmboxNetUuid);
        if (!conn->isOpen())
        {
            result.error = errWith(u8"[KMBOXNET] UDP 连接失败(核对盒子屏幕上的 IP/端口/UUID)",
                                   kmboxNetIp + ":" + kmboxNetPort + " uuid=" + kmboxNetUuid);
            return result;
        }
        result.driver = new OwningDriver<WrappedKmboxNetDriver>(std::move(conn));
        return result;
    }

    result.error = u8"[输入后端] 不认识的名字 \"";
    result.error += backend.empty() ? std::string(u8"(空)") : backend;
    result.error += u8"\", 可选: ";
    const auto names = backendNames();
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i) result.error += " / ";
        result.error += names[i];
    }
    return result;
}

}
