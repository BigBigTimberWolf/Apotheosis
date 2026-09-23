#ifndef MOUSE_H
#define MOUSE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "latest_move_slot.h"
#include "mouse_driver.h"
#include "weapon_switch31.h"

class MakcuConnection;
class MakcuNewConnection;
class KmboxNetConnection;

struct MouseRuntimeParams
{
    int detection_resolution = 320;
};

class MouseThread
{
public:
    struct MovementFeedback
    {
        int dx = 0;
        int dy = 0;
        double latency_ms = 0.0;
        size_t backlog = 0;
        unsigned long long failed = 0;
    };

    MouseThread(
        const MouseRuntimeParams& params,
        MakcuConnection* makcuConnection = nullptr,
        MakcuNewConnection* makcuNewConnection = nullptr,
        KmboxNetConnection* kmboxNetConnection = nullptr,
        MakcuNewConnection* makcuNewConnectionKbd = nullptr);
    ~MouseThread();

    MouseThread(const MouseThread&) = delete;
    MouseThread& operator=(const MouseThread&) = delete;

    void updateParams(const MouseRuntimeParams& params);

    void clearQueuedMoves();
    MovementFeedback consumeMovementFeedback();

    std::recursive_mutex input_method_mutex;

    void sendRawMove(int dx, int dy, int64_t capture_ns = 0, int64_t aim_ns = 0);
    void pressLeftButton();
    void releaseLeftButton();
    void pressRightButton();
    void releaseRightButton();

    bool tapKey(int hid_key, int hold_ms);
    bool requestWeaponSwitch31(int after_shot_delay_ms, int step_ms);
    bool weaponSwitch31Busy() const;

    // 瞬时屏蔽【真实键盘输入】 duration_ms 毫秒(不注入任何键)。
    //
    // 自动急停用: 触发时把玩家按住的方向键"冻住", 让角色停住, 而不是注入
    // 一个反向键去刹车。命令从【键盘那台 MAKCUNEW 硬件】发出 —— 真实键盘
    // 插在它上面, 只有它的屏蔽窗口才有意义。
    // 返回 false 表示当前后端不支持(非 MAKCUNEW)或写串口失败。
    bool maskRealKeyboard(int duration_ms);

    bool supports(uint32_t capability) const;
    uint32_t driverCapabilities() const;
    std::string driverName() const;
    std::string driverStatus() const;

    void setMakcuConnection(MakcuConnection* makcu);
    void setMakcuNewConnection(MakcuNewConnection* makcuNew);
    void setMakcuNewKbdConnection(MakcuNewConnection* makcuNewKbd);
    void setKmboxNetConnection(KmboxNetConnection* kmboxNet);

private:
    void moveWorkerLoop();
    void queueMove(int dx, int dy, int64_t capture_ns, int64_t aim_ns);
    bool sendMovementToDriver(int dx, int dy);

    void sendLeftDownToDriver();
    void sendLeftUpToDriver();
    void sendRightDownToDriver();
    void sendRightUpToDriver();

    void refreshDriver();

    MouseRuntimeParams params_{};
    std::mutex outputMtx_;

    mouse_async::LatestMoveSlot moveSlot_;
    std::mutex queueMtx_;
    std::condition_variable queueCv_;
    std::thread moveWorker_;
    std::unique_ptr<mouse_async::WeaponSwitch31> weaponSwitch31_;
    std::atomic<bool> workerStop_{ false };
    std::mutex feedbackMtx_;
    long long appliedDx_ = 0;
    long long appliedDy_ = 0;
    std::atomic<long long> lastLatencyUs_{ 0 };
    std::atomic<unsigned long long> failedMoves_{ 0 };

    MakcuConnection* makcu_ = nullptr;
    MakcuNewConnection* makcu_new_ = nullptr;
    // 键盘那台 MAKCUNEW(第二台硬件)。nullptr = 未配置, tapKey 回落到 makcu_new_。
    MakcuNewConnection* makcu_new_kbd_ = nullptr;
    KmboxNetConnection* kmbox_net_ = nullptr;

    mouse_driver::IDriver* driver_ = nullptr;
    std::unique_ptr<mouse_driver::IDriver> driver_owned_;
};

#endif // MOUSE_H
