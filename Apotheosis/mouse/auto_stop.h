#ifndef MOUSE_AUTO_STOP_H
#define MOUSE_AUTO_STOP_H

#include <algorithm>
#include <cstdint>

namespace boss
{

class AutoStopController
{
public:
    static constexpr int kHidA = 0x04;
    static constexpr int kHidD = 0x07;
    static constexpr int kHidS = 0x16;
    static constexpr int kHidW = 0x1A;

    static constexpr int64_t kInjectionGuardMs = 25;

    struct Keys
    {
        bool forward = false;
        bool back = false;
        bool left = false;
        bool right = false;

        bool any() const { return forward || back || left || right; }
        void clearByHidKey(int hid_key)
        {
            if (hid_key == kHidW) forward = false;
            else if (hid_key == kHidS) back = false;
            else if (hid_key == kHidA) left = false;
            else if (hid_key == kHidD) right = false;
        }
    };

    struct Action
    {
        bool        tap = false;
        int         hid_key = 0;
        const char* name = "";
    };

    Action tick(bool fired, const Keys& raw, bool enabled, int tap_ms, int64_t now_ms)
    {
        if (!enabled || tap_ms <= 0)
            return {};

        if (holdingFor(now_ms, tap_ms) > 0)
            return {};

        if (!fired)
            return {};

        Keys keys = raw;
        if (last_hid_key_ != 0 &&
            (now_ms - last_tap_ms_) < static_cast<int64_t>(tap_ms) + kInjectionGuardMs)
        {
            keys.clearByHidKey(last_hid_key_);
        }

        Action action;
        if (keys.forward)      { action = {true, kHidS, "S"}; }
        else if (keys.back)    { action = {true, kHidW, "W"}; }
        else if (keys.left)    { action = {true, kHidD, "D"}; }
        else if (keys.right)   { action = {true, kHidA, "A"}; }
        else                   { return {}; }

        last_hid_key_ = action.hid_key;
        last_tap_ms_ = now_ms;
        return action;
    }

    void reset()
    {
        last_hid_key_ = 0;
        last_tap_ms_ = -1000000;
        mask_until_ms_ = 0;
    }

    bool active(int64_t now_ms, int tap_ms) const { return holdingFor(now_ms, tap_ms) > 0; }
    int  lastHidKey() const { return last_hid_key_; }

    // ---- 屏蔽模式(当前自动急停使用) ----
    //
    // 语义已从"注入反向键刹车"改为"屏蔽真实键盘输入 N 毫秒"。
    // 固件侧屏蔽窗口自带硬超时(上限 2000ms), 这里只做本地记账, 避免在窗口内
    // 重复下发 km.mask —— 重复下发虽被固件拒绝重入, 但会在链路上产生无谓流量。
    bool maskActive(int64_t now_ms) const
    {
        return mask_until_ms_ != 0 && now_ms < mask_until_ms_;
    }

    void markMasked(int64_t now_ms, int duration_ms)
    {
        mask_until_ms_ = now_ms + std::max<int64_t>(0, duration_ms);
    }

private:
    int64_t holdingFor(int64_t now_ms, int tap_ms) const
    {
        if (last_hid_key_ == 0)
            return 0;
        const int64_t left = static_cast<int64_t>(tap_ms) - (now_ms - last_tap_ms_);
        return std::max<int64_t>(0, left);
    }

    int     last_hid_key_ = 0;
    int64_t last_tap_ms_ = -1000000;
    int64_t mask_until_ms_ = 0;   // 屏蔽模式: 本地记账的窗口截止时刻
};

}

#endif // MOUSE_AUTO_STOP_H
