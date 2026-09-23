#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

#include "mouse.h"
#include "capture.h"
#include "Apotheosis.h"
#include "Makcu.h"
#include "MakcuNew.h"
#include "kmboxNetConnection.h"
#include "mouse_driver.h"
#include "runtime/latency_probe.h"

namespace
{

MouseRuntimeParams sanitize(MouseRuntimeParams p)
{
    if (p.detection_resolution < 32) p.detection_resolution = 32;
    return p;
}

}

MouseThread::MouseThread(
    const MouseRuntimeParams& params,
    MakcuConnection* makcuConnection,
    MakcuNewConnection* makcuNewConnection,
    KmboxNetConnection* kmboxNetConnection,
    MakcuNewConnection* makcuNewConnectionKbd)
    : makcu_(makcuConnection),
      makcu_new_(makcuNewConnection),
      makcu_new_kbd_(makcuNewConnectionKbd),
      kmbox_net_(kmboxNetConnection)
{
    updateParams(params);
    refreshDriver();
    moveWorker_ = std::thread(&MouseThread::moveWorkerLoop, this);
}

MouseThread::~MouseThread()
{
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        workerStop_.store(true);
        moveSlot_.clear();
    }
    queueCv_.notify_all();
    if (moveWorker_.joinable())
        moveWorker_.join();
}

void MouseThread::sendLeftDownToDriver()
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (driver_) driver_->leftDown();
}

void MouseThread::sendLeftUpToDriver()
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (driver_) driver_->leftUp();
}

void MouseThread::sendRightDownToDriver()
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (driver_) driver_->rightDown();
}

void MouseThread::sendRightUpToDriver()
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (driver_) driver_->rightUp();
}

void MouseThread::updateParams(const MouseRuntimeParams& in)
{
    const auto sanitized = sanitize(in);
    params_ = sanitized;
}

void MouseThread::queueMove(int dx, int dy, int64_t capture_ns, int64_t aim_ns)
{
    if (dx == 0 && dy == 0)
        return;

    const auto valid_component = [](int value) {
        return static_cast<std::uint32_t>(value) + 30000u <= 60000u;
    };
    if (!valid_component(dx) || !valid_component(dy))
        return;

    std::lock_guard<std::mutex> lg(queueMtx_);
    moveSlot_.replace(dx, dy, std::chrono::steady_clock::now(), capture_ns, aim_ns);
    queueCv_.notify_one();
}

void MouseThread::moveWorkerLoop()
{
    try
    {
        while (!workerStop_.load())
        {
            std::unique_lock<std::mutex> ul(queueMtx_);
            queueCv_.wait(ul, [&] {
                return workerStop_.load() || moveSlot_.hasPending();
            });
            if (workerStop_.load())
                break;

            mouse_async::PendingMove move;
            if (!moveSlot_.take(move))
                continue;
            ul.unlock();

            if (!moveSlot_.isCurrent(move.generation))
                continue;

            const bool sent = sendMovementToDriver(move.dx, move.dy);
            if (sent) {
                runtime::latency::markMoveSent(move.capture_ns, move.aim_ns);
                std::lock_guard<std::mutex> feedbackLock(feedbackMtx_);
                appliedDx_ += move.dx;
                appliedDy_ += move.dy;
            } else {
                failedMoves_.fetch_add(1, std::memory_order_release);
            }
            const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - move.queued_at).count();
            lastLatencyUs_.store(latency, std::memory_order_release);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Mouse] Move worker crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[Mouse] Move worker crashed: unknown exception." << std::endl;
    }
}

void MouseThread::sendRawMove(int dx, int dy, int64_t capture_ns, int64_t aim_ns)
{
    if (dx == 0 && dy == 0) return;
    {
        std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
        if (driver_ && driver_->directSend())
        {
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = driver_->move(dx, dy);
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            lastLatencyUs_.store(static_cast<long long>(elapsed_us),
                                 std::memory_order_release);
            if (ok)
            {
                if (dx != 0 || dy != 0) runtime::latency::markMoveSent(capture_ns, aim_ns);
                std::lock_guard<std::mutex> feedbackLock(feedbackMtx_);
                appliedDx_ += dx;
                appliedDy_ += dy;
            }
            else
            {
                failedMoves_.fetch_add(1, std::memory_order_release);
            }
            return;
        }
    }
    queueMove(dx, dy, capture_ns, aim_ns);
}

void MouseThread::pressLeftButton()
{
    sendLeftDownToDriver();
}

void MouseThread::releaseLeftButton()
{
    sendLeftUpToDriver();
}

void MouseThread::pressRightButton()
{
    sendRightDownToDriver();
}

void MouseThread::releaseRightButton()
{
    sendRightUpToDriver();
}

bool MouseThread::tapKey(int hid_key, int hold_ms)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (!driver_ || !driver_->capabilities()) return false;
    return driver_->tapKey(hid_key, hold_ms);
}

bool MouseThread::maskRealKeyboard(int duration_ms)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (!driver_) return false;
    return driver_->maskRealKeyboard(duration_ms);
}

bool MouseThread::supports(uint32_t capability) const
{
    return (driverCapabilities() & capability) != 0;
}

uint32_t MouseThread::driverCapabilities() const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(input_method_mutex));
    return driver_ ? driver_->capabilities() : mouse_driver::kCapNone;
}

std::string MouseThread::driverName() const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(input_method_mutex));
    return driver_ ? driver_->name() : u8"(无)";
}

std::string MouseThread::driverStatus() const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(input_method_mutex));
    if (!driver_)
        return u8"鼠标后端 (未选择) 不可用: 输入方式没有对应到任何已支持的后端";
    if (!driver_->isOpen())
        return mouse_driver::describeStatus(driver_->name(), false, driver_->lastError());
    return mouse_driver::describeStatus(
        driver_->name(), true,
        std::string(u8"能力: ") + mouse_driver::describeCapabilities(driver_->capabilities()));
}

bool MouseThread::sendMovementToDriver(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return true;

    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (!driver_) return false;
    return driver_->move(dx, dy);
}

void MouseThread::clearQueuedMoves()
{
    std::lock_guard<std::mutex> lock(queueMtx_);
    moveSlot_.clear();
    std::lock_guard<std::recursive_mutex> inputLock(input_method_mutex);
    if (driver_) driver_->cancelMove();
}

MouseThread::MovementFeedback MouseThread::consumeMovementFeedback()
{
    MovementFeedback out;
    {
        std::lock_guard<std::mutex> feedbackLock(feedbackMtx_);
        out.dx = static_cast<int>(appliedDx_);
        out.dy = static_cast<int>(appliedDy_);
        appliedDx_ = 0;
        appliedDy_ = 0;
    }
    out.latency_ms = static_cast<double>(
        lastLatencyUs_.load(std::memory_order_acquire)) / 1000.0;
    out.failed = failedMoves_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        out.backlog = moveSlot_.pendingCount();
    }
    return out;
}

void MouseThread::refreshDriver()
{
    driver_owned_.reset();
    driver_ = nullptr;

    if (makcu_ && makcu_new_kbd_)
    {
        // 混合模式: 鼠标那台走【官方 SDK / MakcuConnection】(纯 ASCII, 对齐本轮
        // 重写的新鼠标固件); 键盘那台继续走【MakcuNewConnection】(二进制,
        // 对齐 KBD_PASSTHROUGH 键盘固件, 本轮未动)。
        driver_owned_ = std::make_unique<mouse_driver::WrappedHybridDriver>(
            makcu_, makcu_new_kbd_);
        driver_ = driver_owned_.get();
    }
    else if (makcu_)
    {
        driver_owned_ = std::make_unique<mouse_driver::WrappedMakcuDriver>(makcu_);
        driver_ = driver_owned_.get();
    }
    else if (makcu_new_)
    {
        // 双硬件: 位移/按键/滚轮走 makcu_new_(鼠标那台), 键盘动作走 makcu_new_kbd_。
        // makcu_new_kbd_ 为 nullptr 时驱动内部回落, 行为与单硬件版本一致。
        driver_owned_ = std::make_unique<mouse_driver::WrappedMakcuNewDriver>(
            makcu_new_, makcu_new_kbd_);
        driver_ = driver_owned_.get();
    }
    else if (kmbox_net_)
    {
        driver_owned_ = std::make_unique<mouse_driver::WrappedKmboxNetDriver>(kmbox_net_);
        driver_ = driver_owned_.get();
    }
}

void MouseThread::setMakcuConnection(MakcuConnection* newMakcu)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    makcu_ = newMakcu;
    refreshDriver();
}

void MouseThread::setMakcuNewConnection(MakcuNewConnection* newMakcu)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    makcu_new_ = newMakcu;
    refreshDriver();
}

void MouseThread::setMakcuNewKbdConnection(MakcuNewConnection* newMakcuKbd)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    makcu_new_kbd_ = newMakcuKbd;
    refreshDriver();
}

void MouseThread::setKmboxNetConnection(KmboxNetConnection* newKmboxNet)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    kmbox_net_ = newKmboxNet;
    refreshDriver();
}
