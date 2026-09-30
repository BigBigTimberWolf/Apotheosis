#include <iostream>
#include <thread>
#include <chrono>

#include "kmbox_net/kmboxNet.h"
#include "KmboxNetConnection.h"

KmboxNetConnection::KmboxNetConnection(const std::string& ip, const std::string& port, const std::string& uuid)
    : is_open_(false), ip_(ip), port_(port), uuid_(uuid), monitor_(false)
{
    int ret = 0;
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        ret = kmNet_init((char*)ip.c_str(), (char*)port.c_str(), (char*)uuid.c_str());
    }
    is_open_ = (ret == 0);
    if (!is_open_)
    {
        switch (ret)
        {
        case err_net_invalid_config:
            last_error_ = u8"IP/端口无效，或 UUID 不是 8 位十六进制数"; break;
        case err_net_rx_timeout:
            last_error_ = u8"UDP 握手超时，检查盒子 IP、端口与网络连接"; break;
        case err_net_cmd:
        case err_net_pts:
            last_error_ = u8"盒子回包与当前 KMBoxNet 协议不匹配"; break;
        case err_net_tx:
            last_error_ = u8"UDP 发送失败"; break;
        default:
            last_error_ = u8"网络套接字创建或初始化失败"; break;
        }
        std::cerr << "[KmboxNet] Connection failed, ret=" << ret
                  << " wsa=" << kmNet_last_socket_error()
                  << " endpoint=" << ip_ << ":" << port_
                  << " reason=" << last_error_ << std::endl;
        return;
    }

    aiming_active = false;
    shooting_active = false;
    zooming_active = false;

    monitor_ = false;
    if (monitor_thread_.joinable())
        monitor_thread_.join();

    monitor_thread_ = std::thread(&KmboxNetConnection::monitorThread, this);
}

void KmboxNetConnection::monitorThread()
{
    try
    {
        int ret = 0;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            ret = kmNet_monitor(10000);
        }
        if (ret != 0)
            std::cerr << "[KmboxNet] Physical input monitor failed, ret=" << ret
                      << " wsa=" << kmNet_last_socket_error()
                      << ". Check local UDP port 10000 and firewall; box hotkeys are unavailable."
                      << std::endl;

        while (monitor_running_)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    catch (const std::exception& e)
    {
        std::cerr << "[KmboxNet] Monitor thread crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[KmboxNet] Monitor thread crashed: unknown exception." << std::endl;
    }
}

KmboxNetConnection::~KmboxNetConnection()
{
    monitor_running_ = false;
    if (monitor_thread_.joinable())
        monitor_thread_.join();
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (is_open_) {
        hotkeyKeys_.reset(); automaticKeys_.reset(); hotkeyButtons_.reset();
        if (!syncHotkeyMasksLocked()) syncHotkeyMasksLocked();
        if(ownedAxisMasks_[0] && kmNet_mask_mouse_x(0)!=0) kmNet_mask_mouse_x(0);
        if(ownedAxisMasks_[1] && kmNet_mask_mouse_y(0)!=0) kmNet_mask_mouse_y(0);
        kmNet_monitor(0);
    }
    kmNet_close();
}

bool KmboxNetConnection::maskPhysicalAxis(int axis, bool enabled) {
    if(axis<0 || axis>1 || !is_open_) return false;
    std::lock_guard<std::mutex> lock(io_mutex_);
    if(!enabled && !ownedAxisMasks_[axis]) return true;
    if(enabled) ownedAxisMasks_.set(axis);
    const bool ok=(axis==0 ? kmNet_mask_mouse_x(enabled) : kmNet_mask_mouse_y(enabled))==0;
    if(ok && !enabled) ownedAxisMasks_.reset(axis);
    return ok;
}

void KmboxNetConnection::move(int x, int y)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    int ret = kmNet_mouse_move((short)x, (short)y);
    if (ret != 0)
        std::cerr << "[KmboxNet] Move failed, ret=" << ret << " dx=" << x << " dy=" << y << std::endl;
}

void KmboxNetConnection::moveAuto(int x, int y, int ms)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mouse_move_auto(x, y, ms);
}

void KmboxNetConnection::moveBezier(int x, int y, int ms, int x1, int y1, int x2, int y2)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mouse_move_beizer(x, y, ms, x1, y1, x2, y2);
}

void KmboxNetConnection::leftDown()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ |= 0x01;
    kmNet_mouse_left(1);
}

void KmboxNetConnection::leftUp()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ &= ~0x01;
    kmNet_mouse_left(0);
}

void KmboxNetConnection::rightDown()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ |= 0x02;
    kmNet_mouse_right(1);
}

void KmboxNetConnection::rightUp()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ &= ~0x02;
    kmNet_mouse_right(0);
}

void KmboxNetConnection::middleDown()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ |= 0x04;
    kmNet_mouse_middle(1);
}

void KmboxNetConnection::middleUp()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ &= ~0x04;
    kmNet_mouse_middle(0);
}

void KmboxNetConnection::side1Down()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ |= 0x08;
    kmNet_mouse_all(button_mask_, 0, 0, 0);
}

void KmboxNetConnection::side1Up()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ &= ~0x08;
    kmNet_mouse_all(button_mask_, 0, 0, 0);
}

void KmboxNetConnection::side2Down()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ |= 0x10;
    kmNet_mouse_all(button_mask_, 0, 0, 0);
}

void KmboxNetConnection::side2Up()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ &= ~0x10;
    kmNet_mouse_all(button_mask_, 0, 0, 0);
}

void KmboxNetConnection::wheel(int wheel)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mouse_wheel(wheel);
}

void KmboxNetConnection::mouseAll(int button, int x, int y, int wheel)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    button_mask_ = button & 0x1F;
    kmNet_mouse_all(button, x, y, wheel);
}

bool KmboxNetConnection::keyDown(int hidUsage)
{
    if (!is_open_) return false;
    std::lock_guard<std::mutex> lock(io_mutex_);
    return kmNet_keydown(hidUsage) == 0;
}

bool KmboxNetConnection::keyUp(int hidUsage)
{
    if (!is_open_) return false;
    std::lock_guard<std::mutex> lock(io_mutex_);
    return kmNet_keyup(hidUsage) == 0;
}

void KmboxNetConnection::monitor(short port)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_monitor(port);
}

int KmboxNetConnection::monitorMouseLeft()
{
    if (!is_open_) return -1;
    return kmNet_monitor_mouse_left();
}

int KmboxNetConnection::monitorMouseRight()
{
    if (!is_open_) return -1;
    return kmNet_monitor_mouse_right();
}

int KmboxNetConnection::monitorMouseMiddle()
{
    if (!is_open_) return -1;
    return kmNet_monitor_mouse_middle();
}

int KmboxNetConnection::monitorMouseSide1()
{
    if (!is_open_) return -1;
    return kmNet_monitor_mouse_side1();
}

int KmboxNetConnection::monitorMouseSide2()
{
    if (!is_open_) return -1;
    return kmNet_monitor_mouse_side2();
}

int KmboxNetConnection::monitorKeyboard(short vkey)
{
    if (!is_open_) return -1;
    return kmNet_monitor_keyboard(vkey);
}

void KmboxNetConnection::maskMouseLeft(bool enable)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mask_mouse_left(enable ? 1 : 0);
}
void KmboxNetConnection::maskMouseRight(bool enable)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mask_mouse_right(enable ? 1 : 0);
}
void KmboxNetConnection::maskMouseMiddle(bool enable)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mask_mouse_middle(enable ? 1 : 0);
}
void KmboxNetConnection::maskMouseSide1(bool enable)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mask_mouse_side1(enable ? 1 : 0);
}
void KmboxNetConnection::maskMouseSide2(bool enable)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mask_mouse_side2(enable ? 1 : 0);
}
void KmboxNetConnection::maskMouseX(bool enable)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mask_mouse_x(enable ? 1 : 0);
}
void KmboxNetConnection::maskMouseY(bool enable)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mask_mouse_y(enable ? 1 : 0);
}
void KmboxNetConnection::maskMouseWheel(bool enable)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_mask_mouse_wheel(enable ? 1 : 0);
}
bool KmboxNetConnection::maskKeyboard(short vkey)
{
    if (!is_open_ || vkey <= 0 || vkey >= 256) return false;
    std::lock_guard<std::mutex> lock(io_mutex_);
    automaticKeys_.set(vkey);
    return syncKeyMaskLocked(vkey);
}
bool KmboxNetConnection::unmaskKeyboard(short vkey)
{
    if (!is_open_ || vkey <= 0 || vkey >= 256) return false;
    std::lock_guard<std::mutex> lock(io_mutex_);
    automaticKeys_.reset(vkey);
    return syncKeyMaskLocked(vkey);
}
void KmboxNetConnection::unmaskAll()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    hotkeyKeys_.reset(); automaticKeys_.reset(); hotkeyButtons_.reset();
    if (kmNet_unmask_all() == 0) {
        appliedKeys_.reset(); uncertainKeys_.reset();
        appliedButtons_.reset(); uncertainButtons_.reset();
    } else {
        uncertainKeys_ |= appliedKeys_; uncertainButtons_ |= appliedButtons_;
    }
}

bool KmboxNetConnection::syncKeyMaskLocked(int hid)
{
    const bool desired = hotkeyKeys_[hid] || automaticKeys_[hid];
    if (appliedKeys_[hid] == desired && !uncertainKeys_[hid]) return true;
    // A lost acknowledgement may still have changed the box. Keep ownership
    // until a matching unmask is acknowledged, including during destruction.
    uncertainKeys_.set(hid);
    const int result = desired ? kmNet_mask_keyboard(static_cast<short>(hid))
                               : kmNet_unmask_keyboard(static_cast<short>(hid));
    if (result != 0) return false;
    appliedKeys_[hid] = desired; uncertainKeys_.reset(hid);
    return true;
}

bool KmboxNetConnection::syncHotkeyMasksLocked()
{
    bool ok = true;
    for (int hid = 1; hid < 256; ++hid) ok = syncKeyMaskLocked(hid) && ok;
    for (int button = 1; button <= 5; ++button) {
        const bool desired = hotkeyButtons_[button];
        if (appliedButtons_[button] == desired && !uncertainButtons_[button]) continue;
        uncertainButtons_.set(button);
        int result = -1;
        switch (button) {
        case 1: result = kmNet_mask_mouse_left(desired); break;
        case 2: result = kmNet_mask_mouse_right(desired); break;
        case 3: result = kmNet_mask_mouse_middle(desired); break;
        case 4: result = kmNet_mask_mouse_side1(desired); break;
        case 5: result = kmNet_mask_mouse_side2(desired); break;
        }
        if (result == 0) { appliedButtons_[button] = desired; uncertainButtons_.reset(button); }
        else ok = false;
    }
    return ok;
}

bool KmboxNetConnection::setHotkeyMasks(const std::bitset<256>& keys, const std::bitset<6>& buttons,
                                      bool force, bool* pendingRelease)
{
    if (!is_open_) return false;
    std::lock_guard<std::mutex> lock(io_mutex_);
    bool pending = false;
    for (int hid = 1; hid < 256; ++hid) {
        if (!force && hotkeyKeys_[hid] != keys[hid] && kmNet_monitor_keyboard(static_cast<short>(hid)) > 0)
            pending = true;
        else hotkeyKeys_[hid] = keys[hid];
    }
    const int mouseState[] = {0, kmNet_monitor_mouse_left(), kmNet_monitor_mouse_right(),
        kmNet_monitor_mouse_middle(), kmNet_monitor_mouse_side1(), kmNet_monitor_mouse_side2()};
    for (int button = 1; button <= 5; ++button) {
        if (!force && hotkeyButtons_[button] != buttons[button] && mouseState[button] > 0)
            pending = true;
        else hotkeyButtons_[button] = buttons[button];
    }
    if (pendingRelease) *pendingRelease = pending;
    return syncHotkeyMasksLocked() && !pending;
}

void KmboxNetConnection::reboot()
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_reboot();
    is_open_ = false;
}

void KmboxNetConnection::setConfig(const std::string& ip, unsigned short port)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_setconfig((char*)ip.c_str(), port);
}

void KmboxNetConnection::debug(short port, char enable)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_debug(port, enable);
}

void KmboxNetConnection::lcdColor(unsigned short rgb565)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_lcd_color(rgb565);
}
void KmboxNetConnection::lcdPictureBottom(unsigned char* buff_128_80)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_lcd_picture_bottom(buff_128_80);
}
void KmboxNetConnection::lcdPicture(unsigned char* buff_128_160)
{
    if (!is_open_) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    kmNet_lcd_picture(buff_128_160);
}
