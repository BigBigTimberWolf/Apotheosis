#ifndef MOUSE_AUTO_STOP_H
#define MOUSE_AUTO_STOP_H

#include <algorithm>
#include <cstdint>

namespace boss
{

// 自动急停: 开火那一拍屏蔽真实键盘输入 N 毫秒。
//
// 语义已从"注入反向键刹车"改为"屏蔽真实键盘输入"。固件侧屏蔽窗口自带硬超时
// (上限 2000ms), 这里只做本地记账, 避免在窗口内重复下发 km.mask —— 重复下发
// 虽被固件拒绝重入, 但会在链路上产生无谓流量。
class AutoStopController
{
public:
    void reset()
    {
        mask_until_ms_ = 0;
    }

    bool maskActive(int64_t now_ms) const
    {
        return mask_until_ms_ != 0 && now_ms < mask_until_ms_;
    }

    void markMasked(int64_t now_ms, int duration_ms)
    {
        mask_until_ms_ = now_ms + std::max<int64_t>(0, duration_ms);
    }

private:
    int64_t mask_until_ms_ = 0;   // 屏蔽模式: 本地记账的窗口截止时刻
};

}

#endif // MOUSE_AUTO_STOP_H
