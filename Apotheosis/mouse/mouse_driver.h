#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class MakcuConnection;
class MakcuNewConnection;
class KmboxNetConnection;

namespace mouse_driver
{

enum Capability : uint32_t
{
    kCapNone         = 0,
    kCapMove         = 1u << 0,
    kCapButtonLeft   = 1u << 1,
    kCapButtonRight  = 1u << 2,
    kCapButtonMiddle = 1u << 3,
    kCapButtonSide   = 1u << 4,
    kCapWheel        = 1u << 5,
    kCapKeyboard     = 1u << 6,
    kCapPhysicalRead = 1u << 7,
    kCapKeyboardMask = 1u << 8,
};

inline const char* capabilityName(uint32_t cap)
{
    switch (cap)
    {
    case kCapMove:         return u8"位移";
    case kCapButtonLeft:   return u8"左键";
    case kCapButtonRight:  return u8"右键";
    case kCapButtonMiddle: return u8"中键";
    case kCapButtonSide:   return u8"侧键";
    case kCapWheel:        return u8"滚轮";
    case kCapKeyboard:     return u8"键盘";
    case kCapPhysicalRead: return u8"物理按键回读";
    case kCapKeyboardMask: return u8"真实键盘屏蔽";
    default:               return u8"未知";
    }
}

std::string describeCapabilities(uint32_t caps);

class IDriver
{
public:
    virtual ~IDriver() = default;

    virtual const char* name() const = 0;

    virtual uint32_t capabilities() const = 0;

    virtual bool isOpen() const = 0;

    virtual std::string lastError() const = 0;

    virtual bool move(int dx, int dy) = 0;

    virtual bool leftDown() = 0;
    virtual bool leftUp() = 0;
    virtual bool rightDown() = 0;
    virtual bool rightUp() = 0;
    virtual bool middleDown() = 0;
    virtual bool middleUp() = 0;

    virtual bool button(int b, bool down) {
        switch(b) {
        case 1: return down ? leftDown() : leftUp();
        case 2: return down ? rightDown() : rightUp();
        case 3: return down ? middleDown() : middleUp();
        default: return false;
        }
    }
    virtual bool wheel(int  ) { return false; }

    virtual bool tapKey(int  , int  , int   = 0) { return false; }
    virtual bool keyDown(int) { return false; }
    virtual bool keyUp(int) { return false; }

    // 屏蔽真实键盘输入(毫秒, <=0 表示解除)。后端可选择屏蔽的按键范围。
    virtual bool maskRealKeyboard(int  ) { return false; }
    virtual bool keyboardMaskExpires() const { return true; }

    virtual int physicalButtonPressed(int  ) const { return -1; }
    virtual int physicalKeyPressed(int) const { return -1; }
    virtual bool maskPhysicalButton(int, bool) { return false; }
    virtual bool maskPhysicalKey(int, bool) { return false; }
    virtual bool maskPhysicalAxis(int, bool) { return false; } // 0=X, 1=Y; physical input only.

    virtual void cancelMove() {}

    virtual bool directSend() const { return false; }
};

class DriverConnectionHolder
{
public:
    virtual ~DriverConnectionHolder() = default;
};

template <typename ConnT>
class DriverConnection final : public DriverConnectionHolder
{
public:
    explicit DriverConnection(std::unique_ptr<ConnT> conn) : conn_(std::move(conn)) {}
    ConnT* get() const { return conn_.get(); }

private:
    std::unique_ptr<ConnT> conn_;
};

// 双连接持有者: 给 MAKCUNEW 的"鼠标一台 + 键盘一台"用。
// 两个槽位各自独立释放, 顺序无关紧要(两个连接之间没有引用关系)。
template <typename ConnT>
class DriverConnectionPair final : public DriverConnectionHolder
{
public:
    DriverConnectionPair(std::unique_ptr<ConnT> primary, std::unique_ptr<ConnT> secondary)
        : primary_(std::move(primary)), secondary_(std::move(secondary)) {}
    ConnT* primary() const { return primary_.get(); }
    ConnT* secondary() const { return secondary_.get(); }

private:
    std::unique_ptr<ConnT> primary_;
    std::unique_ptr<ConnT> secondary_;
};

template <typename WrapperT>
class OwningDriver final : public IDriver
{
public:
    template <typename ConnT>
    explicit OwningDriver(std::unique_ptr<ConnT> conn)
        : holder_(std::make_unique<DriverConnection<ConnT>>(std::move(conn))),
          wrapper_(static_cast<DriverConnection<ConnT>*>(holder_.get())->get())
    {
    }

    // 双连接版本(MAKCUNEW 专用): wrapper 由 (primary, secondary) 两块连接构造。
    template <typename ConnT>
    OwningDriver(std::unique_ptr<ConnT> primary, std::unique_ptr<ConnT> secondary)
        : holder_(std::make_unique<DriverConnectionPair<ConnT>>(
              std::move(primary), std::move(secondary))),
          wrapper_(static_cast<DriverConnectionPair<ConnT>*>(holder_.get())->primary(),
                   static_cast<DriverConnectionPair<ConnT>*>(holder_.get())->secondary())
    {
    }

    const char* name() const override { return wrapper_.name(); }
    uint32_t capabilities() const override { return wrapper_.capabilities(); }
    bool isOpen() const override { return wrapper_.isOpen(); }
    std::string lastError() const override { return wrapper_.lastError(); }
    bool move(int dx, int dy) override { return wrapper_.move(dx, dy); }
    bool leftDown() override { return wrapper_.leftDown(); }
    bool leftUp() override { return wrapper_.leftUp(); }
    bool rightDown() override { return wrapper_.rightDown(); }
    bool rightUp() override { return wrapper_.rightUp(); }
    bool middleDown() override { return wrapper_.middleDown(); }
    bool middleUp() override { return wrapper_.middleUp(); }
    bool wheel(int delta) override { return wrapper_.wheel(delta); }
    bool tapKey(int hidKey, int holdMs, int mod) override { return wrapper_.tapKey(hidKey, holdMs, mod); }
    bool maskRealKeyboard(int durationMs) override { return wrapper_.maskRealKeyboard(durationMs); }
    int physicalButtonPressed(int button) const override { return wrapper_.physicalButtonPressed(button); }
    void cancelMove() override { wrapper_.cancelMove(); }
    bool directSend() const override { return wrapper_.directSend(); }

private:
    std::unique_ptr<DriverConnectionHolder> holder_;
    WrapperT wrapper_;
};

class WrappedMakcuDriver final : public IDriver
{
public:
    explicit WrappedMakcuDriver(MakcuConnection* conn);
    const char* name() const override;
    uint32_t capabilities() const override;
    bool isOpen() const override;
    std::string lastError() const override;
    bool move(int dx, int dy) override;
    bool leftDown() override;
    bool leftUp() override;
    bool rightDown() override;
    bool rightUp() override;
    bool middleDown() override;
    bool middleUp() override;
    bool wheel(int delta) override;
    bool tapKey(int hidKey, int holdMs, int mod) override;
    int physicalButtonPressed(int button) const override;
private:
    MakcuConnection* conn_;
};

class WrappedMakcuNewDriver final : public IDriver
{
public:
    // 双硬件(方案 A): 鼠标动作走 conn_mouse_, 键盘动作走 conn_kbd_。
    //
    // 为什么拆开: 本项目的部署是"一台硬件接鼠标、一台接键盘", 两台各自独立
    // 接入被控机。原实现只有一个连接, 位移/按键/滚轮/键盘全挤在同一条链路上,
    // 于是真实键盘所在的硬件无法与鼠标硬件同时使用。
    //
    // conn_kbd_ 允许为 null(此时键盘动作回落到鼠标那台, 保持单硬件可用),
    // 也允许与 conn_mouse_ 相同(单硬件同时插键鼠的旧场景)。
    WrappedMakcuNewDriver(MakcuNewConnection* connMouse, MakcuNewConnection* connKbd);
    const char* name() const override;
    uint32_t capabilities() const override;
    bool isOpen() const override;
    std::string lastError() const override;
    bool move(int dx, int dy) override;
    bool leftDown() override;
    bool leftUp() override;
    bool rightDown() override;
    bool rightUp() override;
    bool middleDown() override;
    bool middleUp() override;
    bool wheel(int delta) override;
    bool tapKey(int hidKey, int holdMs, int mod) override;
    bool maskRealKeyboard(int durationMs) override;
    int physicalButtonPressed(int button) const override;
    void cancelMove() override;
    bool directSend() const override;

    // 键盘硬件(第二台)。未接入时返回 nullptr —— 【不回落】到鼠标那台。
    //
    // 为什么不回落: 键盘事件送给鼠标硬件是有害的。鼠标那台的 C 口插的是鼠标,
    // 往它发键盘报文/屏蔽命令不但无效, 还会干扰它自己的真实鼠标状态(屏蔽命令
    // 在那台上会连带把真实鼠标输入一起屏蔽掉)。因此"没接键盘硬件"必须表现为
    // 【键盘功能不可用】, 而不是把键盘动作重定向到鼠标硬件上。
    MakcuNewConnection* keyboardConnection() const { return conn_kbd_; }
private:
    MakcuNewConnection* conn_mouse_;
    MakcuNewConnection* conn_kbd_;
};

// 混合驱动: 鼠标动作走【官方 SDK / MakcuConnection】(纯 ASCII 协议), 键盘动作与
// 屏蔽真实输入走【MakcuNewConnection】(二进制协议, 与 KBD_PASSTHROUGH 键盘固件对齐)。
//
// 为什么这么拆:
//   · 鼠标那台的固件已经在本轮重写为纯 ASCII 命令 (km.move/km.left(1)/...), 官方 SDK
//     内部发的正好是这套命令, 无缝对接。
//   · 键盘那台的固件 (KBD_PASSTHROUGH) 本轮不动, 继续用 MakcuNewConnection 的
//     二进制帧 (CMD_KEY_TAP / CMD_MASK 等) 发键盘 tap 与屏蔽命令。
//
// kbdConn 允许为 nullptr —— 只有鼠标那台时行为等价于 WrappedMakcuDriver, 键盘能力
// 位不声明; tapKey/maskRealKeyboard 直接返回失败。
class WrappedHybridDriver final : public IDriver
{
public:
    WrappedHybridDriver(MakcuConnection* mouseConn, MakcuNewConnection* kbdConn);
    const char* name() const override;
    uint32_t capabilities() const override;
    bool isOpen() const override;
    std::string lastError() const override;
    bool move(int dx, int dy) override;
    bool leftDown() override;
    bool leftUp() override;
    bool rightDown() override;
    bool rightUp() override;
    bool middleDown() override;
    bool middleUp() override;
    bool wheel(int delta) override;
    bool tapKey(int hidKey, int holdMs, int mod) override;
    bool maskRealKeyboard(int durationMs) override;
    int physicalButtonPressed(int button) const override;
    MakcuNewConnection* keyboardConnection() const { return kbdConn_; }
private:
    MakcuConnection* mouseConn_;
    MakcuNewConnection* kbdConn_;
};

class WrappedKmboxNetDriver final : public IDriver
{
public:
    explicit WrappedKmboxNetDriver(KmboxNetConnection* conn);
    ~WrappedKmboxNetDriver() override;
    const char* name() const override;
    uint32_t capabilities() const override;
    bool isOpen() const override;
    std::string lastError() const override;
    bool move(int dx, int dy) override;
    bool leftDown() override;
    bool leftUp() override;
    bool rightDown() override;
    bool rightUp() override;
    bool middleDown() override;
    bool middleUp() override;
    bool wheel(int delta) override;
    bool tapKey(int hidKey, int holdMs, int mod) override;
    bool maskRealKeyboard(int durationMs) override;
    bool keyboardMaskExpires() const override { return false; }
    int physicalButtonPressed(int button) const override;
    bool directSend() const override;
private:
    KmboxNetConnection* conn_;
    std::array<bool, 4> movementKeysMasked_{};
    bool movementMaskUncertain_ = false;
};

struct OpenResult
{
    IDriver* driver = nullptr;
    std::string error;
};

extern const char* const kBackendMakcu;
extern const char* const kBackendMakcuNew;
extern const char* const kBackendKmboxNet;
extern const char* const kBackendFerrum;
extern const char* const kBackendDhzboxMini;
extern const char* const kBackendCpbox;

std::vector<std::string> backendNames();

OpenResult open(const std::string& backend,
                const std::string& makcuPort, unsigned int makcuBaud,
                const std::string& makcuNewPort, unsigned int makcuNewBaud,
                const std::string& kmboxNetIp, const std::string& kmboxNetPort,
                const std::string& kmboxNetUuid,
                // 第二台 MAKCUNEW(键盘)。空 = 未配置, 键盘动作回落到 makcuNewPort。
                const std::string& makcuNewPortKbd = "",
                unsigned int makcuNewBaudKbd = 6000000,
                const std::string& ferrumPort = "", unsigned int ferrumBaud = 3000000,
                const std::string& dhzboxIp = "", unsigned short dhzboxPort = 8888, int dhzboxKey = 88,
                const std::string& catIp = "", unsigned short catPort = 8888,
                const std::string& catUuid = "", unsigned short catMonitorPort = 1234,
                const std::string& cpboxPort = "");

std::string describeStatus(const std::string& backend, bool open, const std::string& detail);

}
