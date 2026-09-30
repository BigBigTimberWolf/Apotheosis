#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include "macro/macro_engine.h"
#include "macro/macro_config.h"
#include "Apotheosis.h"
#include "keyboard/keyboard_listener.h"
#include "keyboard/keycodes.h"
#include "keyboard/hotkey_blocking.h"
#include "mouse/kmboxNetConnection.h"
#include "mouse/windows_driver.h"
#include "runtime/config_snapshot.h"
#include "runtime/active_hotkey.h"
#include "runtime/aim_loop.h"
#include <chrono>
#include <map>
#include <random>
#include <set>
#include <utility>

namespace macros {
namespace {
using Clock = std::chrono::steady_clock;
std::mutex stateMutex;
std::recursive_mutex outputLock;
std::atomic<bool> outputOwned{false};
std::atomic<bool> devicesChanging{false};
Status currentStatus;
std::vector<Program> programs;
std::shared_ptr<const Config> seenConfig;
std::string configPath, backend, stopKey, request;
bool master = false, editing = false, rearm = true;
int paused = 0;
std::map<std::string,bool> previousKeys;
std::mutex flashRequestMutex;
std::string flashRequest;
int flashRequestHotkey = -1;
bool flashCancelRequested = false;
struct FlashPulse {
    bool active = false;
    bool keyboard = false;
    int code = 0;
    int aimHotkeyIndex = -1;
    Clock::time_point deadline{};
} flash;
struct Job {
    Program program;
    size_t index = 0;
    bool active = false, once = false, waiting = false, stepDone = false;
    bool moved = false;
    Clock::time_point deadline{};
    // false = mouse channel, true = HID keyboard usage.
    std::set<std::pair<bool,int>> held;
    std::pair<bool,int> pulse{};
    bool hasPulse = false;
} job;
std::mt19937 randomEngine{std::random_device{}()};

enum class Output { Move, Button, Key, Wheel };
bool send(Output op,int a,int b=0) {
    std::lock_guard<std::mutex> device(inputDeviceMutex);
    if(backend=="WINDOWS") {
        if(!windowsDriver || !windowsDriver->isOpen()) return false;
        switch(op) {
        case Output::Move: return windowsDriver->move(a,b);
        case Output::Wheel: return windowsDriver->wheel(a);
        case Output::Button: return windowsDriver->button(a,b!=0);
        case Output::Key: return b ? windowsDriver->keyDown(a) : windowsDriver->keyUp(a);
        }
    }
    if(op==Output::Key) {
        if(backend=="CAT" || backend=="FERRUM") {
            auto driver=backend=="CAT" ? catDriver : ferrumDriver;
            return driver && driver->isOpen() && (b ? driver->keyDown(a) : driver->keyUp(a));
        }
        if(backend=="KMBOXNET") return kmboxNetSerial && kmboxNetSerial->isOpen() &&
            (b ? kmboxNetSerial->keyDown(a) : kmboxNetSerial->keyUp(a));
        if((backend!="MAKCU" && backend!="MAKCUNEW") || !makcuNewSerialKbd || !makcuNewSerialKbd->isOpen()) return false;
        uint8_t mods=0; std::array<uint8_t,6> keys{}; size_t n=0;
        for(const auto& h:job.held) {
            if(!h.first || (!b && h.second==a)) continue;
            if(h.second>=224 && h.second<=231) mods|=static_cast<uint8_t>(1u<<(h.second-224));
            else { if(n==keys.size()) return false; keys[n++]=static_cast<uint8_t>(h.second); }
        }
        if(flash.active && flash.keyboard && (b || flash.code!=a)) {
            if(flash.code>=224 && flash.code<=231) mods|=static_cast<uint8_t>(1u<<(flash.code-224));
            else if(!job.held.count({true,flash.code})) {
                if(n==keys.size()) return false;
                keys[n++]=static_cast<uint8_t>(flash.code);
            }
        }
        return makcuNewSerialKbd->sendKeyboardReport(mods,keys);
    }
    if(backend=="MAKCU" && makcuSerial && makcuSerial->isOpen()) {
        if(op==Output::Move) makcuSerial->move(a,b);
        else if(op==Output::Wheel) makcuSerial->wheel(a);
        else if(op==Output::Button) { if(b) makcuSerial->press(a); else makcuSerial->release(a); }
        return makcuSerial->isOpen();
    }
    if(backend=="MAKCUNEW" && makcuNewSerial && makcuNewSerial->isOpen()) {
        if(op==Output::Move) return makcuNewSerial->move(a,b);
        if(op==Output::Button) return b ? makcuNewSerial->press(a) : makcuNewSerial->release(a);
        if(op==Output::Wheel) { makcuNewSerial->wheel(a); return makcuNewSerial->isOpen(); }
    }
    if(backend=="KMBOXNET" && kmboxNetSerial && kmboxNetSerial->isOpen()) {
        if(op==Output::Move) kmboxNetSerial->move(a,b);
        else if(op==Output::Wheel) kmboxNetSerial->wheel(a);
        else if(op==Output::Button) {
            switch(a) {
            case 1: b ? kmboxNetSerial->leftDown() : kmboxNetSerial->leftUp(); break;
            case 2: b ? kmboxNetSerial->rightDown() : kmboxNetSerial->rightUp(); break;
            case 3: b ? kmboxNetSerial->middleDown() : kmboxNetSerial->middleUp(); break;
            case 4: b ? kmboxNetSerial->side1Down() : kmboxNetSerial->side1Up(); break;
            case 5: b ? kmboxNetSerial->side2Down() : kmboxNetSerial->side2Up(); break;
            default: return false;
            }
        }
        return kmboxNetSerial->isOpen();
    }
    auto driver=backend=="FERRUM" ? ferrumDriver : backend=="DHZBOX_MINI" ? dhzboxDriver : backend=="CAT" ? catDriver : nullptr;
    if(!driver || !driver->isOpen()) return false;
    if(op==Output::Move) return driver->move(a,b);
    if(op==Output::Wheel) return driver->wheel(a);
    if(op==Output::Button) return driver->button(a,b!=0);
    return false;
}
bool button(bool keyboard,int code,bool down) {
    const auto key=std::make_pair(keyboard,code);
    if(down && job.held.count(key)) return true;
    if(!down && !job.held.count(key)) return true; // Never release another owner's input.
    // Retain uncertain downs for a best-effort release after transport failure.
    if(down) job.held.insert(key);
    const bool ok=send(keyboard ? Output::Key : Output::Button,code,down ? 1 : 0);
    if(ok && !down) job.held.erase(key);
    return ok;
}
void releaseFlash() {
    if(!flash.active) return;
    std::lock_guard<std::recursive_mutex> output(outputLock);
    const auto old=flash;
    flash.active=false;
    send(old.keyboard ? Output::Key : Output::Button,old.code,0);
}
void beginFlash(const std::string& key, int aimHotkeyIndex) {
    if(flash.active || job.active || devicesChanging.load()) return;
    bool keyboard=true;
    int code=hidKey(key);
    if(!code) {
        keyboard=false;
        if(key=="LeftMouseButton") code=1;
        else if(key=="RightMouseButton") code=2;
        else if(key=="MiddleMouseButton") code=3;
        else if(key=="X1MouseButton") code=4;
        else if(key=="X2MouseButton") code=5;
    }
    if(code==0 || job.held.count({keyboard,code})) return;
    std::lock_guard<std::recursive_mutex> output(outputLock);
    flash.active=true;
    flash.keyboard=keyboard;
    flash.code=code;
    flash.aimHotkeyIndex=aimHotkeyIndex;
    if(!send(keyboard ? Output::Key : Output::Button,code,1)) {
        flash.active=false;
        return;
    }
    flash.deadline=Clock::now()+std::chrono::milliseconds(40);
}
void finish(const std::string& message) {
    std::lock_guard<std::recursive_mutex> output(outputLock);
    bool released=true;
    const auto held=job.held;
    for(const auto& h:held) {
        released=send(h.first ? Output::Key : Output::Button,h.second,0) && released;
        job.held.erase(h);
    }
    if(job.active) runtime::aim_loop::finishMacroControl(job.moved);
    job={}; outputOwned=false;
    currentStatus.running=false;
    currentStatus.message=message;
    if(!released) currentStatus.message+=u8"；设备未确认松键，请检查连接";
}
bool pressed(const std::string& key) {
    const int vk=KeyCodes::getKeyCode(key);
    if(key.empty() || vk<=0) return false;
    if(backend=="WINDOWS") return mouse_driver::windowsPhysicalKeyPressed(vk);
    const int hid=hidKey(key);
    if(hid) {
        if(mouse_driver::windowsPhysicalKeyPressed(vk)) return true;
        std::lock_guard<std::mutex> device(inputDeviceMutex);
        if(backend=="FERRUM") return ferrumDriver && ferrumDriver->physicalKeyPressed(hid)>0;
        if(backend=="CAT") return catDriver && catDriver->physicalKeyPressed(hid)>0;
        return backend=="KMBOXNET" && kmboxNetSerial && kmboxNetSerial->isOpen() &&
            kmboxNetSerial->monitorKeyboard(static_cast<short>(hid))>0;
    }
    if((GetAsyncKeyState(vk)&0x8000)!=0) return true;
    return isAnyKeyPressed({key});
}
bool condition(const Program& p,int resolution) {
    if(!p.targetOnly) return true;
    std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
    const auto age=Clock::now()-detectionBuffer.stamp;
    if(detectionBuffer.stamp==Clock::time_point{} || age>std::chrono::milliseconds(600) ||
        detectionBuffer.staleLocked()) return false;
    for(size_t i=0;i<detectionBuffer.boxes.size() && i<detectionBuffer.classes.size();++i) {
        if(!p.classes.empty() && std::find(p.classes.begin(),p.classes.end(),
            detectionBuffer.classes[i])==p.classes.end()) continue;
        const double h=100.0*detectionBuffer.boxes[i].height/std::max(1,resolution);
        if(!p.heightFilter || (h>=p.minHeightPercent && h<=p.maxHeightPercent)) return true;
    }
    return false;
}
std::string validate(const Program& p) {
    if(p.actions.empty()) return u8"请先添加动作";
    std::set<int> keyboardHeld;
    std::set<int> mouseHeld;
    for(const auto& a:p.actions) {
        if(static_cast<int>(a.type)<0 || static_cast<int>(a.type)>11) return u8"存在无法识别的动作";
        if(a.type==ActionType::KeyDown || a.type==ActionType::KeyUp || a.type==ActionType::KeyPress) {
            const int key=hidKey(a.key);
            if(!key) return u8"动作中的键盘按键无效";
            if(a.type==ActionType::KeyPress && keyboardHeld.count(key))
                return u8"不能点按宏已按住的同一个键；请先添加松开动作";
            if(a.type==ActionType::KeyUp) keyboardHeld.erase(key);
            else keyboardHeld.insert(key);
            if(std::count_if(keyboardHeld.begin(),keyboardHeld.end(),[](int k){ return k<224; })>6)
                return u8"同一时间最多支持按住 6 个普通键（Ctrl、Shift 等修饰键另计）";
            if(a.type==ActionType::KeyPress) keyboardHeld.erase(key);
            std::lock_guard<std::mutex> device(inputDeviceMutex);
            if(backend=="WINDOWS") {
                if(!windowsDriver || !windowsDriver->isOpen()) return u8"Windows 原生输入尚未启用";
            } else if(backend=="KMBOXNET") {
                if(!kmboxNetSerial || !kmboxNetSerial->isOpen()) return u8"KMBox Net 未连接";
            } else if(backend=="CAT" || backend=="FERRUM") {
                auto driver=backend=="CAT" ? catDriver : ferrumDriver;
                if(!driver || !driver->isOpen() || !(driver->capabilities() & mouse_driver::kCapKeyboard))
                    return u8"当前设备接口不支持键盘输出或尚未连接";
            } else if((backend!="MAKCU" && backend!="MAKCUNEW") ||
                !makcuNewSerialKbd || !makcuNewSerialKbd->isOpen()) return u8"请先连接 KMBox Net 或使用本项目键盘固件的独立 MAKCU 键盘设备";
        }
        if(a.type==ActionType::MouseDown || a.type==ActionType::MouseUp || a.type==ActionType::MouseClick) {
            if(a.a<1 || a.a>5) return u8"鼠标按键无效";
            if(a.type==ActionType::MouseClick && mouseHeld.count(a.a))
                return u8"不能点击宏已按住的同一个鼠标键；请先添加松开动作";
            if(a.type==ActionType::MouseDown) mouseHeld.insert(a.a);
            if(a.type==ActionType::MouseUp) mouseHeld.erase(a.a);

        }
        if(a.type==ActionType::MouseDown || a.type==ActionType::MouseUp || a.type==ActionType::MouseClick ||
            a.type==ActionType::MouseMove || a.type==ActionType::Wheel) {
            std::lock_guard<std::mutex> device(inputDeviceMutex);
            bool connected=false;
            if(backend=="WINDOWS") connected=windowsDriver && windowsDriver->isOpen();
            else if(backend=="MAKCU") connected=makcuSerial && makcuSerial->isOpen();
            else if(backend=="MAKCUNEW") connected=makcuNewSerial && makcuNewSerial->isOpen();
            else if(backend=="KMBOXNET") connected=kmboxNetSerial && kmboxNetSerial->isOpen();
            else if(backend=="CAT") connected=catDriver && catDriver->isOpen();
            else if(backend=="FERRUM") connected=ferrumDriver && ferrumDriver->isOpen();
            else if(backend=="DHZBOX_MINI") connected=dhzboxDriver && dhzboxDriver->isOpen();
            if(!connected) return u8"当前鼠标设备未连接";
            if(backend=="CAT" && a.type==ActionType::Wheel) return u8"当前 CAT 协议未提供滚轮输出接口";
        }
    }
    return {};
}
void start(const Program& p,bool once) {
    const auto error=validate(p);
    if(!error.empty()) { currentStatus.message=error; return; }
    std::lock_guard<std::recursive_mutex> output(outputLock);
    releaseFlash();
    if(!runtime::aim_loop::prepareForMacro()) {
        currentStatus.message=u8"自动切枪正在完成，请稍后重试"; return;
    }
    job={}; job.program=p; job.active=true; job.once=once; job.deadline=Clock::now();
    outputOwned=true;
    currentStatus={true,p.id,u8"正在执行："+p.name,1,static_cast<int>(p.actions.size())};
}
void advance() {
    if(!job.active || job.waiting || Clock::now()<job.deadline) return;
    std::lock_guard<std::recursive_mutex> output(outputLock);
    const auto now=Clock::now();
    if(job.hasPulse) {
        if(!button(job.pulse.first,job.pulse.second,false)) { finish(u8"松键失败，宏已停止"); return; }
        job.hasPulse=false;
    }
    if(job.stepDone) {
        job.stepDone=false;
        ++job.index;
        if(job.index==job.program.actions.size()) {
            if(job.once || job.program.mode==Mode::Once || job.program.mode==Mode::Sequence) {
                finish(u8"执行完成"); return;
            }
            // A loop boundary releases unmatched presses. No stuck key leaks into the next cycle.
            const auto held=job.held;
            for(const auto& h:held) if(!button(h.first,h.second,false)) { finish(u8"松键失败，宏已停止"); return; }
            job.index=0; job.deadline=now+std::chrono::milliseconds(job.program.loopIntervalMs);
            return;
        }
        if(!job.once && job.program.mode==Mode::Sequence) {
            job.waiting=true; currentStatus.message=u8"等待下一次按键 · 下一步 "+std::to_string(job.index+1);
            currentStatus.step=static_cast<int>(job.index+1); return;
        }
    }
    const auto& a=job.program.actions[job.index];
    currentStatus.step=static_cast<int>(job.index+1);
    currentStatus.message=u8"正在执行："+job.program.name;
    bool ok=true;
    int delay=0;
    switch(a.type) {
    case ActionType::Delay: delay=std::uniform_int_distribution<int>(a.a,a.b)(randomEngine); break;
    case ActionType::MouseMove: job.moved=true; ok=send(Output::Move,a.a,a.b); break;
    case ActionType::Wheel: ok=send(Output::Wheel,a.a); break;
    case ActionType::KeyDown: ok=button(true,hidKey(a.key),true); break;
    case ActionType::KeyUp: ok=button(true,hidKey(a.key),false); break;
    case ActionType::MouseDown: ok=button(false,a.a,true); break;
    case ActionType::MouseUp: ok=button(false,a.a,false); break;
    case ActionType::KeyPress:
    case ActionType::MouseClick: {
        const bool keyboard=a.type==ActionType::KeyPress;
        const int code=keyboard ? hidKey(a.key) : a.a;
        if(job.held.count({keyboard,code})) { finish(u8"不能点按宏已按住的同一个键；请先添加松开动作"); return; }
        delay=a.b;
        ok=button(keyboard,code,true); job.hasPulse=ok; job.pulse={keyboard,code};
        break;
    }
    case ActionType::PidReset: runtime::aim_loop::resetPidAxes(true,true); break;
    case ActionType::PidResetX: runtime::aim_loop::resetPidAxes(true,false); break;
    case ActionType::PidResetY: runtime::aim_loop::resetPidAxes(false,true); break;
    default: ok=false;
    }
    if(!ok) { finish(u8"动作发送失败，宏已停止；请检查设备连接和动作支持情况"); return; }
    job.stepDone=true;
    job.deadline=Clock::now()+std::chrono::milliseconds(delay);
}
} // namespace

std::recursive_mutex& outputMutex() { return outputLock; }
bool ownsOutput() { return outputOwned.load() || devicesChanging.load(); }
Status status() { std::lock_guard<std::mutex> lock(stateMutex); return currentStatus; }
void stopAll() {
    std::lock_guard<std::mutex> lock(stateMutex);
    releaseFlash();
    finish(u8"已停止；松开触发键后可重新触发"); request.clear(); rearm=true;
}
void shutdown() {
    std::lock_guard<std::mutex> lock(stateMutex);
    cancelAutoFlash();
    releaseFlash();
    finish(u8"宏已关闭"); request.clear(); rearm=true;
    hotkey_blocking::clear();
}
void setEditing(bool value) {
    std::lock_guard<std::mutex> lock(stateMutex);
    if(editing==value) return;
    editing=value;
    if(value) { finish(u8"编排中：热键暂停，可点击运行一次"); request.clear(); }
    else if(!job.active) currentStatus.message=u8"等待触发";
    rearm=true;
}
void runOnce(const std::string& id) {
    std::lock_guard<std::mutex> lock(stateMutex);
    request=id;
}
DevicePause::DevicePause() {
    std::lock_guard<std::mutex> lock(stateMutex);
    cancelAutoFlash();
    releaseFlash();
    ++paused; devicesChanging=true; finish(u8"设备正在重新连接"); request.clear(); rearm=true;
    hotkey_blocking::clear();
}
DevicePause::~DevicePause() {
    std::lock_guard<std::mutex> lock(stateMutex);
    --paused; devicesChanging=paused>0; rearm=true;
}

void requestAutoFlash(const std::string& key, int aimHotkeyIndex) {
    std::lock_guard<std::mutex> lock(flashRequestMutex);
    flashRequest=key;
    flashRequestHotkey=aimHotkeyIndex;
    flashCancelRequested=false;
}
void cancelAutoFlash() {
    std::lock_guard<std::mutex> lock(flashRequestMutex);
    flashRequest.clear();
    flashRequestHotkey=-1;
    flashCancelRequested=true;
}

void tick() {
    std::lock_guard<std::mutex> lock(stateMutex);
    if(paused) return;
    const auto cfg=runtime_config::read();
    if(!cfg) return;
    hotkey_blocking::update(cfg,editing);
    if(seenConfig!=cfg && (configPath!=cfg->configPath() || programs!=cfg->macro_programs ||
        master!=cfg->macro_programs_enabled || backend!=cfg->input_method || stopKey!=cfg->macro_stop_key)) {
        const bool differentDeviceOrProfile=configPath!=cfg->configPath() || backend!=cfg->input_method;
        releaseFlash();
        finish(u8"配置已更新，等待触发");
        programs=cfg->macro_programs; configPath=cfg->configPath();
        master=cfg->macro_programs_enabled; backend=cfg->input_method; stopKey=cfg->macro_stop_key;
        previousKeys.clear(); rearm=true; if(differentDeviceOrProfile) request.clear();
    }
    seenConfig=cfg;
    std::string flashKey;
    int flashHotkey=-1;
    bool cancelFlash=false;
    {
        std::lock_guard<std::mutex> requestLock(flashRequestMutex);
        flashKey=std::exchange(flashRequest,std::string{});
        flashHotkey=std::exchange(flashRequestHotkey,-1);
        cancelFlash=std::exchange(flashCancelRequested,false);
    }
    if(cancelFlash || (flash.active &&
        (Clock::now()>=flash.deadline ||
         flash.aimHotkeyIndex!=runtime::g_active_hotkey_index.load()))) releaseFlash();
    if(!cancelFlash && !flashKey.empty() &&
        flashHotkey==runtime::g_active_hotkey_index.load()) beginFlash(flashKey,flashHotkey);
    if(!master) { currentStatus.message=u8"宏编排已关闭"; request.clear(); return; }
    std::map<std::string,bool> down;
    for(const auto& p:programs) if(p.enabled && !p.trigger.empty())
        down.try_emplace(p.trigger,false);
    for(auto& entry:down) entry.second=pressed(entry.first);
    if(pressed(stopKey.empty() ? "F12" : stopKey)) {
        finish(u8"已按停止键，全部宏停止"); request.clear(); rearm=true; previousKeys=down; return;
    }
    const bool suppress=rearm || editing;
    rearm=false;
    if(job.active) {
        if(!condition(job.program,cfg->detection_resolution) ||
            (!job.once && job.program.mode==Mode::Hold && !down[job.program.trigger]))
            finish(u8"触发条件结束，宏已停止");
    }
    const auto manual=std::exchange(request,std::string{});
    for(const auto& p:programs) {
        if(!p.enabled) continue;
        const bool clicked=manual==p.id;
        const bool edge=!suppress && down[p.trigger] && !previousKeys[p.trigger];
        if(!clicked && !edge) continue;
        if(!clicked && std::count_if(programs.begin(),programs.end(),[&](const Program& other){
            return other.enabled && other.trigger==p.trigger;
        })>1) { currentStatus.message=u8"多个宏使用了同一触发键，请在宏编排中修改"; continue; }
        if(!clicked && p.trigger==stopKey) { currentStatus.message=u8"触发键与停止键冲突"; continue; }
        if(job.active) {
            if(job.program.id==p.id && !job.once && p.mode==Mode::Toggle && !clicked)
                finish(u8"循环已停止");
            else if(job.program.id==p.id && job.waiting && !clicked) job.waiting=false;
            else currentStatus.message=u8"已有宏运行；请先停止当前宏";
            break;
        }
        if(!condition(p,cfg->detection_resolution)) { currentStatus.message=u8"等待满足目标条件"; continue; }
        start(p,clicked);
        break;
    }
    previousKeys=std::move(down);
    advance();
}
} // namespace macros
