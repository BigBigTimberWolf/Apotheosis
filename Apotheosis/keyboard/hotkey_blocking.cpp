#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include "keyboard/hotkey_blocking.h"
#include "keyboard/keycodes.h"
#include "keyboard/hotkey_selection.h"
#include "Apotheosis.h"
#include "mouse/windows_driver.h"
#include "mouse/kmboxNetConnection.h"
#include "runtime/active_hotkey.h"
#include "runtime/inference_session.h"
#include "runtime/aim_loop.h"
#include <bitset>
#include <chrono>
#include <mutex>

namespace hotkey_blocking {
namespace {
std::shared_ptr<const Config> previous;
bool previousEditing = false, retry = false;
std::chrono::steady_clock::time_point retryAt{};
std::mutex statusMutex;
std::string message = u8"热键屏蔽：等待输入设备";
std::string maskBackend;
uint64_t makcuNewGeneration=0;
std::string axisMessage=u8"真实轴屏蔽：未启用";
std::string axisBackend;
std::bitset<2> axisApplied, axisUncertain, axisRequested;
uint64_t axisGeneration=0;
std::chrono::steady_clock::time_point axisRetryAt{};
void reportAxes(const std::string& text) {
    std::lock_guard<std::mutex> lock(statusMutex); axisMessage=text;
}
std::bitset<6> hardwareButtons, uncertainButtons;
std::bitset<256> hardwareKeys, uncertainKeys;
std::shared_ptr<mouse_driver::IDriver> otherDriver(const std::string& backend) {
    return backend=="FERRUM" ? ferrumDriver : backend=="DHZBOX_MINI" ? dhzboxDriver : backend=="CAT" ? catDriver : nullptr;
}
bool setButton(const std::string& backend,int b,bool on) {
    if(backend=="MAKCU") return makcuSerial && makcuSerial->maskPhysicalButton(b,on);
    if(backend=="MAKCUNEW") return makcuNewSerial && makcuNewSerial->maskPhysicalButton(b,on);
    auto driver=otherDriver(backend);
    return driver && driver->maskPhysicalButton(b,on);
}
bool setAxis(const std::string& backend,int axis,bool on) {
    if(backend=="MAKCU") return makcuSerial && makcuSerial->maskPhysicalAxis(axis,on);
    if(backend=="MAKCUNEW") return makcuNewSerial && makcuNewSerial->maskPhysicalAxis(axis,on);
    if(backend=="KMBOXNET") return kmboxNetSerial && kmboxNetSerial->maskPhysicalAxis(axis,on);
    auto driver=otherDriver(backend);
    return driver && driver->maskPhysicalAxis(axis,on);
}
bool releaseAxes() { // inputDeviceMutex held; retain uncertain releases for retry.
    bool ok=true;
    for(int a=0;a<2;++a) if(axisApplied[a] || axisUncertain[a]) {
        if(setAxis(axisBackend,a,false)) { axisApplied.reset(a); axisUncertain.reset(a); }
        else ok=false;
    }
    return ok;
}
void updateAxes(const Config& cfg, bool editing) {
    std::bitset<2> wanted;
    const int index=runtime::aim_loop::activeTargetHotkey();
    const bool active=!editing && !session_stop_requested.load() &&
        g_inference_session && g_inference_session->running() &&
        index==runtime::g_active_hotkey_index.load();
    if(active && index>=0 && index<static_cast<int>(cfg.hotkeys.size())) {
        wanted[0]=cfg.hotkeys[index].mask_x;
        wanted[1]=cfg.hotkeys[index].mask_y;
    }
    const auto now=std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(inputDeviceMutex);
    const uint64_t generation=cfg.input_method=="MAKCUNEW" && makcuNewSerial
        ? makcuNewSerial->sessionGeneration() : 0;
    const bool changed=axisBackend!=cfg.input_method || axisRequested!=wanted || axisGeneration!=generation;
    if(!changed && now<axisRetryAt) return;
    axisRequested=wanted;
    axisRetryAt=now+std::chrono::milliseconds(500);
    if(axisBackend!=cfg.input_method) {
        if(!releaseAxes()) {
            reportAxes(u8"真实轴屏蔽：旧设备尚未确认解除，请检查连接"); return;
        }
        axisBackend=cfg.input_method;
    }
    if(axisGeneration!=generation) {
        axisUncertain|=axisApplied;
        axisGeneration=generation;
    }
    if(cfg.input_method=="WINDOWS" || cfg.input_method=="CAT") {
        reportAxes(cfg.input_method=="WINDOWS"
            ? u8"真实轴屏蔽：Windows 原生输入暂不支持；解锁轴正常可用"
            : u8"真实轴屏蔽：当前 CAT 协议未提供轴屏蔽；解锁轴正常可用");
        return;
    }
    bool ok=true;
    for(int a=0;a<2;++a) {
        if(axisApplied[a]==wanted[a] && !axisUncertain[a]) continue;
        axisUncertain.set(a); // A timed-out write may already have reached hardware.
        if(setAxis(axisBackend,a,wanted[a])) { axisApplied[a]=wanted[a]; axisUncertain.reset(a); }
        else ok=false;
    }
    if(!ok) reportAxes(u8"真实轴屏蔽：设备未确认，请检查连接 / 固件是否支持轴锁");
    else if(wanted.none()) reportAxes(u8"真实轴屏蔽：已解除；等待有效目标并开始瞄准");
    else reportAxes(u8"真实轴屏蔽："+axisBackend+u8" 已下发 "+
        (wanted[0] ? "X " : "")+(wanted[1] ? "Y" : "")+u8"；自动瞄准仍可移动");
}
int physicalButton(const std::string& backend,int b) {
    if(backend=="MAKCU" && makcuSerial) {
        switch(b) {
        case 1:return makcuSerial->shooting_active; case 2:return makcuSerial->zooming_active;
        case 3:return makcuSerial->middle_active; case 4:return makcuSerial->side1_active;
        case 5:return makcuSerial->side2_active;
        }
    }
    if(backend=="MAKCUNEW" && makcuNewSerial) return makcuNewSerial->physicalButtonPressed(b);
    auto driver=otherDriver(backend); return driver ? driver->physicalButtonPressed(b) : -1;
}
bool releaseHardware() {
    bool ok=true;
    for(int b=1;b<=5;++b) if(hardwareButtons[b] || uncertainButtons[b]) {
        if(setButton(maskBackend,b,false)) { hardwareButtons.reset(b); uncertainButtons.reset(b); }
        else ok=false;
    }
    auto driver=otherDriver(maskBackend);
    for(int k=1;k<256;++k) if(hardwareKeys[k] || uncertainKeys[k]) {
        if(driver && driver->maskPhysicalKey(k,false)) { hardwareKeys.reset(k); uncertainKeys.reset(k); }
        else ok=false;
    }
    return ok;
}
void report(const std::string& text) {
    std::lock_guard<std::mutex> lock(statusMutex);
    message = text;
}
int buttonCode(const std::string& key) {
    if(key=="LeftMouseButton") return 1;
    if(key=="RightMouseButton") return 2;
    if(key=="MiddleMouseButton") return 3;
    if(key=="X1MouseButton") return 4;
    if(key=="X2MouseButton") return 5;
    return 0;
}
}
std::string status() {
    std::lock_guard<std::mutex> lock(statusMutex);
    return message;
}
std::string axisStatus() {
    std::lock_guard<std::mutex> lock(statusMutex); return axisMessage;
}
void clear() {
    previous.reset(); retry = false;
    mouse_driver::windowsSetBlockedHotkeys({});
    std::lock_guard<std::mutex> lock(inputDeviceMutex);
    bool ok = releaseAxes();
    if(!ok) ok=releaseAxes();
    axisRequested.reset(); axisGeneration=0; axisRetryAt={};
    // DevicePause / shutdown are ownership boundaries. Each old driver retains
    // uncertain locks for its destructor; never apply them to a replacement device.
    axisApplied.reset(); axisUncertain.reset(); axisBackend.clear();
    reportAxes(ok ? u8"真实轴屏蔽：已解除" : u8"真实轴屏蔽：设备未确认解除，请检查连接");
    if(!releaseHardware()) ok=releaseHardware() && ok;
    // Device destructors retain any uncertain masks for a final cleanup attempt.
    hardwareButtons.reset(); uncertainButtons.reset(); hardwareKeys.reset(); uncertainKeys.reset();
    maskBackend.clear();
    makcuNewGeneration=0;
    if(kmboxNetSerial && kmboxNetSerial->isOpen()) {
        bool cleared=kmboxNetSerial->setHotkeyMasks({},{},true);
        if(!cleared) cleared=kmboxNetSerial->setHotkeyMasks({},{},true);
        ok=cleared && ok;
    }
    report(ok ? u8"热键屏蔽：已暂停" : u8"热键屏蔽：设备未确认解除，请检查硬件连接");
}
void update(const std::shared_ptr<const Config>& cfg, bool macroEditing) {
    if(!cfg) return;
    updateAxes(*cfg,macroEditing);
    const auto now = std::chrono::steady_clock::now();
    if(previous==cfg && previousEditing==macroEditing && (!retry || now<retryAt)) return;
    previous=cfg; previousEditing=macroEditing;
    retry=false;
    std::bitset<256> windows, localKeyboard, keys;
    std::bitset<6> buttons;
    auto add = [&](const std::string& key) {
        const int vk=KeyCodes::getKeyCode(key);
        if(vk<=0 || vk>=256) return;
        windows.set(vk);
        if(const int b=buttonCode(key)) buttons.set(b);
        else if(const int hid=macros::hidKey(key)) { keys.set(hid); localKeyboard.set(vk); }
    };
    for (int i = 0; i < static_cast<int>(cfg->hotkeys.size()); ++i) {
        const auto& profile = cfg->hotkeys[i];
        if (profile.block_hotkey && profile.group == cfg->active_hotkey_group &&
            preferredHotkey(cfg->hotkeys, i) == i)
            for (const auto& key : profile.keys) add(key);
    }
    if(cfg->macro_programs_enabled && !macroEditing)
        for(const auto& p:cfg->macro_programs)
            if(p.enabled && p.blockTrigger && !p.actions.empty() && p.trigger!=cfg->macro_stop_key)
                add(p.trigger);

    const bool wanted=windows.any();
    const bool native=cfg->input_method=="WINDOWS";
    const bool net=cfg->input_method=="KMBOXNET";
    const bool hardware=cfg->input_method=="MAKCU" || cfg->input_method=="MAKCUNEW" ||
        cfg->input_method=="FERRUM" || cfg->input_method=="DHZBOX_MINI" || cfg->input_method=="CAT";
    bool ok=true, pending=false;
    bool connected=!wanted, monitored=true;
    {
        std::lock_guard<std::mutex> lock(inputDeviceMutex);
        if(native) connected=windowsDriver && windowsDriver->isOpen();
        if(maskBackend!=cfg->input_method) {
            if(!releaseHardware()) {
                mouse_driver::windowsSetBlockedHotkeys({});
                retry=true; retryAt=now+std::chrono::milliseconds(500);
                report(u8"热键屏蔽：旧设备尚未确认解除，请重新连接设备");
                return;
            }
            maskBackend=cfg->input_method;
        }
        if(hardware) {
            auto driver=otherDriver(cfg->input_method);
            if(cfg->input_method=="MAKCU") connected=makcuSerial && makcuSerial->isOpen();
            else if(cfg->input_method=="MAKCUNEW") connected=makcuNewSerial && makcuNewSerial->isOpen();
            else connected=driver && driver->isOpen();
            if(connected) {
                if(cfg->input_method=="MAKCUNEW" && makcuNewGeneration!=makcuNewSerial->sessionGeneration()) {
                    makcuNewGeneration=makcuNewSerial->sessionGeneration();
                    uncertainButtons|=hardwareButtons;
                }
                for(int b=1;b<=5;++b) {
                    if(hardwareButtons[b]==buttons[b] && !uncertainButtons[b]) continue;
                    if(hardwareButtons[b]!=buttons[b] && physicalButton(cfg->input_method,b)>0) { pending=true; continue; }
                    hardwareButtons[b]=buttons[b]; uncertainButtons.set(b);
                    if(setButton(cfg->input_method,b,buttons[b])) uncertainButtons.reset(b);
                    else ok=false;
                }
                // Ferrum documents per-HID-key masking and pre-mask Keys() callbacks.
                if(cfg->input_method=="FERRUM" || cfg->input_method=="CAT") for(int k=1;k<256;++k) {
                    if(hardwareKeys[k]==keys[k] && !uncertainKeys[k]) continue;
                    if(hardwareKeys[k]!=keys[k] && driver->physicalKeyPressed(k)>0) { pending=true; continue; }
                    hardwareKeys[k]=keys[k]; uncertainKeys.set(k);
                    if(driver->maskPhysicalKey(k,keys[k])) uncertainKeys.reset(k);
                    else ok=false;
                }
            }
        }
        if(kmboxNetSerial && kmboxNetSerial->isOpen()) {
            if(net) {
                connected=true;
                monitored=kmboxNetSerial->monitorMouseLeft()>=0;
            }
            // Never consume the trigger if its physical state cannot be read.
            ok=kmboxNetSerial->setHotkeyMasks(net && monitored ? keys : std::bitset<256>{},
                net && monitored ? buttons : std::bitset<6>{}, !net || !monitored, &pending) && ok;
        }
    }
    ok=mouse_driver::windowsSetBlockedHotkeys(connected ? (native ? windows : localKeyboard) : std::bitset<256>{}) && ok;
    if(wanted && !native && !net && !hardware) {
        report(u8"热键屏蔽：当前设备 / 固件不支持，开关仅保存设置");
        return;
    }
    if(!ok || pending || (wanted && (!connected || !monitored))) {
        retry=true; retryAt=now+std::chrono::milliseconds(pending ? 5 : 500);
        report(pending ? u8"热键屏蔽：请松开原触发键，随后应用新设置"
                       : (cfg->input_method=="MAKCU" || cfg->input_method=="MAKCUNEW") && connected
                           ? u8"热键屏蔽：固件未确认屏蔽，请使用支持按键锁的固件（项目旧固件需更新）"
                           : u8"热键屏蔽：尚未就绪，请检查设备连接和物理按键监听");
        return;
    }
    if(!wanted) report(u8"热键屏蔽：当前没有启用的屏蔽键");
    else if(native) report(u8"热键屏蔽：Windows 本机已启用；本程序前台时放行");
    else if(net) report(u8"热键屏蔽：KMBox 被控端已启用");
    else report(u8"热键屏蔽："+cfg->input_method+u8" 屏蔽命令已下发"+
        (keys.any() && cfg->input_method!="FERRUM" && cfg->input_method!="CAT" ? u8"；键盘热键仅拦截本机键盘，被控机独立键盘未接入逐键屏蔽" : ""));
    // Recheck physical monitoring even when settings did not change. Applying
    // an unchanged mask is local bookkeeping and sends no device commands.
    if(wanted) { retry=true; retryAt=now+std::chrono::milliseconds(500); }
}
}
