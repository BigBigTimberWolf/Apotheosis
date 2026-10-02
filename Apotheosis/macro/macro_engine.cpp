#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include "macro/macro_engine.h"
#include "macro/macro_config.h"
#include "macro/rule_executor.h"
#include "macro/rule_sources.h"
#include "macro/control_directive.h"
#include "runtime/aim_telemetry.h"
#include "config/config_profiles.h"
#include <QApplication>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMetaObject>
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
std::string simulationRequest;
RuleSnapshot latestSnapshot;
std::unique_ptr<RuleExecutor> executor, simulator;
bool master = false, editing = false, rearm = true;
int paused = 0;
int sourceResolution=0;
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
    // Aggregate hardware report; per-rule ownership lives in RuleExecutor.
    std::set<std::pair<bool,int>> held;
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
        if(a>=0x10000)return false;
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
    if(flash.active || outputOwned.load() || devicesChanging.load()) return;
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
    if(executor)executor->stop();if(simulator)simulator->stop();
    stopSourceEffects();
    clearDirective();
    bool released=true;
    const auto held=job.held;
    for(const auto& h:held) {
        released=send(h.first ? Output::Key : Output::Button,h.second,0) && released;
        job.held.erase(h);
    }
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
        if(hid>=0x10000)return false;
        std::lock_guard<std::mutex> device(inputDeviceMutex);
        if(backend=="FERRUM") return ferrumDriver && ferrumDriver->physicalKeyPressed(hid)>0;
        if(backend=="CAT") return catDriver && catDriver->physicalKeyPressed(hid)>0;
        return backend=="KMBOXNET" && kmboxNetSerial && kmboxNetSerial->isOpen() &&
            kmboxNetSerial->monitorKeyboard(static_cast<short>(hid))>0;
    }
    if((GetAsyncKeyState(vk)&0x8000)!=0) return true;
    return isAnyKeyPressed({key});
}
void systemResult(const std::string& text){std::lock_guard<std::mutex> lock(stateMutex);currentStatus.message=text;if(executor)executor->report(text);}
std::shared_future<std::string> beginSystemAction(const Action& a,std::shared_ptr<std::atomic<bool>> alive) {
    if(a.type!=ActionType::Notification&&a.type!=ActionType::SwitchProfile&&a.type!=ActionType::ExportConfig&&a.type!=ActionType::ImportConfig)return {};
    auto promise=std::make_shared<std::promise<std::string>>();auto future=promise->get_future().share();
    if(!qApp){promise->set_value(u8"界面尚未就绪");return future;}
        QMetaObject::invokeMethod(qApp,[a,promise,alive]{
            if(!alive->load()){promise->set_value(u8"动作已取消");return;}
            const auto text=QString::fromUtf8(a.text.c_str());QString error;bool ok=true;
            if(a.type==ActionType::Notification){auto* box=new QMessageBox(QMessageBox::Information,QStringLiteral("宏通知"),text,QMessageBox::Ok);box->setTextFormat(Qt::PlainText);box->setAttribute(Qt::WA_DeleteOnClose);box->open();}
            else if(a.type==ActionType::SwitchProfile)ok=ConfigProfiles::instance().switchTo(text,&error);
            else if(a.type==ActionType::ExportConfig){Config copy;{std::lock_guard<std::recursive_mutex> l(configMutex);copy=config;}ok=copy.saveConfig(a.text);if(!ok)error=QStringLiteral("导出文件失败");}
            else {const QFileInfo file(text);auto name=ConfigProfiles::sanitizeName(file.completeBaseName());
                const auto destination=QDir(ConfigProfiles::instance().directory()).filePath(name+".ini");
                if(name.isEmpty()||QFile::exists(destination)){ok=false;error=QStringLiteral("导入名称为空或与已有方案重名，请先重命名文件");}
                else if(!QFile::copy(text,destination)){ok=false;error=QStringLiteral("无法读取或复制配置文件");}
                else {ConfigProfiles::instance().refresh();ok=ConfigProfiles::instance().switchTo(name,&error);}
            }
            const auto result=ok?std::string{}:u8"系统动作失败："+error.toUtf8().toStdString();
            if(!ok)systemResult(result);promise->set_value(result);
        },Qt::QueuedConnection);return future;

}
bool hostAction(const Action& a,const RuleSnapshot& s,std::string& error) {
    switch(a.type) {
    case ActionType::MouseMove:return send(Output::Move,a.a,a.b);
    case ActionType::Wheel:return send(Output::Wheel,a.a);
    case ActionType::PidReset:runtime::aim_loop::resetPidAxes(true,true);return true;
    case ActionType::PidResetX:runtime::aim_loop::resetPidAxes(true,false);return true;
    case ActionType::PidResetY:runtime::aim_loop::resetPidAxes(false,true);return true;
    case ActionType::Text:
        if(backend!="WINDOWS"){error=u8"文本输入需要 Windows 原生输出；硬件请使用键盘动作 / 组合键";return false;}
        return mouse_driver::windowsTypeText(a.text);
    case ActionType::AbsoluteMove:
        if(backend!="WINDOWS"){error=u8"绝对桌面坐标需要 Windows 原生输出；硬件支持相对移动";return false;}
        return mouse_driver::windowsMoveAbsolute(a.a,a.b);
    case ActionType::MoveToTarget:case ActionType::MoveToPrediction: {
        auto it=std::find_if(s.targets.begin(),s.targets.end(),[&](const Target& t){return t.id==s.targetId;});
        if(it==s.targets.end()){error=u8"目标已丢失";return false;}
        if(!(a.value>0)){error=u8"请设置每像素对应设备计数，不能直接混用图像像素和鼠标计数";return false;}
        auto get=[&](const char* k,double fallback){auto v=s.values.find(k);return v!=s.values.end()&&v->second.known?v->second.number:fallback;};
        const double seconds=a.type==ActionType::MoveToPrediction?std::clamp(a.c,0,500)/1000.:0;
        return send(Output::Move,int(std::clamp((it->x+a.a+it->vx*seconds-get("cross.x",s.width*.5))*a.value,-32767.,32767.)),
            int(std::clamp((it->y+a.b+it->vy*seconds-get("cross.y",s.height*.5))*a.value,-32767.,32767.)));
    }
    case ActionType::LockTarget:case ActionType::NextTarget: {
        if(s.targets.empty()){error=u8"没有可选择的目标";return false;}
        auto it=std::find_if(s.targets.begin(),s.targets.end(),[&](const Target& t){return t.id==s.targetId;});
        if(a.type==ActionType::NextTarget&&it!=s.targets.end())++it;if(it==s.targets.end())it=s.targets.begin();
        changeDirective([&](ControlDirective& d){d.commandSerial=d.revision+1;d.command=a.type==ActionType::LockTarget?1:3;d.x=it->x;d.y=it->y;});return true;
    }
    case ActionType::UnlockTarget:case ActionType::ClearTarget:
        changeDirective([&](ControlDirective& d){d.commandSerial=d.revision+1;d.command=a.type==ActionType::UnlockTarget?2:4;});return true;
    case ActionType::AimPart:case ActionType::AimClass:case ActionType::AimPriority:
    case ActionType::Prediction:case ActionType::Smoothing:case ActionType::SpeedLimit:
        changeDirective([&](ControlDirective& d){
            if(a.type==ActionType::AimPart){d.partX=std::clamp(a.a/100.,0.,1.);d.partY=std::clamp(a.b/100.,0.,1.);}
            if(a.type==ActionType::AimClass)d.classId=a.a;
            if(a.type==ActionType::AimPriority)d.priority=std::clamp(a.a,0,4);
            if(a.type==ActionType::Prediction)d.predictionMs=std::clamp(a.value,0.,500.);
            if(a.type==ActionType::Smoothing)d.smoothing=std::clamp(a.value,.01,1.);
            if(a.type==ActionType::SpeedLimit)d.speed=std::max(0.,a.value);
        });return true;
    case ActionType::Sound:case ActionType::Vibration:return sourceAction(a,s,error);
    default:error=u8"动作尚未受支持";return false;
    }
}
void ensureExecutor() {
    if(executor)return;
    RuleHost host;
    host.button=[](bool keyboard,int code,bool down){return button(keyboard,code,down);};
    host.action=hostAction;host.asyncAction=beginSystemAction;
    host.acquire=[] {releaseFlash();const auto orphaned=job.held;for(const auto& key:orphaned)if(!button(key.first,key.second,false))return false;
        if(!runtime::aim_loop::prepareForMacro())return false;outputOwned=true;return true;};
    host.release=[](bool moved){runtime::aim_loop::finishMacroControl(moved);outputOwned=false;};
    executor=std::make_unique<RuleExecutor>(host);
    simulator=std::make_unique<RuleExecutor>(RuleHost{});
}
RuleSnapshot snapshot(const Config& cfg) {
    RuleSnapshot s;s.now=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    s.width=s.height=cfg.detection_resolution;
    static int version=-1,nextId=1;static int64_t previousTime=0,lockStarted=0;static int previousTarget=-1;
    static std::vector<Target> tracks;
    {
        std::lock_guard<std::mutex> l(detectionBuffer.mutex);
        const bool fresh=detectionBuffer.stamp!=Clock::time_point{}&&Clock::now()-detectionBuffer.stamp<std::chrono::milliseconds(600)&&!detectionBuffer.staleLocked();
        const auto currentCross=detectionBuffer.frame_crosshair.forDetection(detectionBuffer.frame_context,runtime::g_active_hotkey_index.load(),cfg.detection_resolution);
        s.values["color.hit"]=Value::numeric(fresh&&currentCross.has_value());
        if(fresh&&currentCross){s.values["cross.x"]=Value::numeric(currentCross->x);s.values["cross.y"]=Value::numeric(currentCross->y);}
        if(fresh&&version!=detectionBuffer.version){
            version=detectionBuffer.version;std::vector<Target> next;std::set<int> used;
            const auto frameMs=detectionBuffer.frame_stamp_ns>0?detectionBuffer.frame_stamp_ns/1000000:s.now;
            const double dt=(frameMs-previousTime)/1000.;
            for(size_t i=0;i<detectionBuffer.boxes.size()&&i<detectionBuffer.classes.size();++i){
                const auto& b=detectionBuffer.boxes[i];Target t;t.x=b.x+b.width*.5;t.y=b.y+b.height*.5;t.width=b.width;t.height=b.height;t.classId=detectionBuffer.classes[i];
                t.confidence=i<detectionBuffer.confidences.size()?detectionBuffer.confidences[i]:0;
                const Target* nearest=nullptr;double best=std::max(40.,std::hypot(t.width,t.height));
                if(dt>0&&dt<.6)for(const auto& old:tracks)if(old.classId==t.classId&&!used.count(old.id)){
                    const double distance=std::hypot(t.x-old.x,t.y-old.y);if(distance<best){best=distance;nearest=&old;}}
                if(nearest){t.id=nearest->id;used.insert(t.id);t.vx=(t.x-nearest->x)/dt;t.vy=(t.y-nearest->y)/dt;}else t.id=nextId++;
                next.push_back(t);
            }tracks=std::move(next);previousTime=frameMs;
        }
        if(fresh)s.targets=tracks;else tracks.clear();
    }
    const auto overlay=runtime::readAimOverlay();
    const bool fresh=Clock::now()-overlay.ts<std::chrono::milliseconds(200);
    double best=1e30;for(const auto& t:s.targets){const double distance=std::hypot(t.x-(fresh&&overlay.valid?overlay.filtered_cx:s.width*.5),t.y-(fresh&&overlay.valid?overlay.filtered_cy:s.height*.5));if(distance<best){best=distance;s.targetId=t.id;}}
    if(outputOwned.load())for(const auto& t:s.targets)if(t.id==latestSnapshot.targetId){s.targetId=t.id;break;}
    s.values["aim.active"]=Value::numeric(fresh&&overlay.engaged);
    s.values["trigger.active"]=Value::numeric(fresh&&overlay.trigger_reason==runtime::TriggerOverlayReason::Pressed);
    s.values["aim.key"]=Value::numeric(runtime::g_active_hotkey_index.load()>=0);
    static int64_t lastVisible=0;static int lastVisibleId=-1;
    if(s.targetId>=0){lastVisible=s.now;lastVisibleId=s.targetId;}
    s.values["target.occluded"]=Value::numeric(s.targetId<0&&lastVisibleId>=0&&s.now-lastVisible<=150);
    const int active=runtime::g_active_hotkey_index.load();
    double rx=fresh?overlay.fov_radius_x:0,ry=fresh?overlay.fov_radius_y:0;
    if((rx<=0||ry<=0)&&active>=0&&active<int(cfg.hotkeys.size())){rx=cfg.hotkeys[active].fovX*.5;ry=cfg.hotkeys[active].fovY*.5;}
    const auto crossx=s.values.find("cross.x"),crossy=s.values.find("cross.y");
    const double cx=crossx==s.values.end()?s.width*.5:crossx->second.number,cy=crossy==s.values.end()?s.height*.5:crossy->second.number;
    bool within=false;for(const auto& t:s.targets)if(rx>0&&ry>0)within|=std::pow(std::max(0.,std::abs(t.x-cx)-t.width*.5)/rx,2)+std::pow(std::max(0.,std::abs(t.y-cy)-t.height*.5)/ry,2)<=1;
    s.values["target.fov"]=Value::numeric(within);
    s.values["target.locked"]=Value::numeric(fresh&&overlay.valid);
    if(s.targetId!=previousTarget){previousTarget=s.targetId;lockStarted=s.now;}
    s.values["target.lock_ms"]=Value::numeric(s.targetId<0?0:s.now-lockStarted);
    // Output ownership suspends the aim loop; this suspension itself is not an aim-stop event.
    if(outputOwned.load())for(const char* name:{"aim.active","trigger.active","target.locked"}){
        auto it=latestSnapshot.values.find(name);if(it!=latestSnapshot.values.end())s.values[name]=active>=0?it->second:Value::numeric(0);
    }
    std::set<std::string> wanted;
    for(const auto& p:programs){for(const auto& step:split(p.trigger,'>'))for(const auto& key:split(step,'+'))wanted.insert(key);
        for(const auto& c:p.conditions)if(c.metric=="key")for(const auto& key:split(c.text,'+'))wanted.insert(key);}
    for(const auto& key:wanted)if(pressed(key))s.keys.insert(key);
    static uint64_t wheelUp=0,wheelDown=0;
    const auto up=mouse_driver::windowsWheelCounter(true),down=mouse_driver::windowsWheelCounter(false);
    if(up!=wheelUp)s.keys.insert("WheelUp");if(down!=wheelDown)s.keys.insert("WheelDown");wheelUp=up;wheelDown=down;
    s.values["random.percent"]=Value::numeric(std::uniform_real_distribution<double>(0,100)(randomEngine));
    s.values["profile"]=Value::string(cfg.configPath());appendSources(s);return s;
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
    stopSources();
    hotkey_blocking::clear();
}
void setEditing(bool value) {
    std::lock_guard<std::mutex> lock(stateMutex);
    if(editing==value) return;
    editing=value;
    if(value) { finish(u8"编排中：热键暂停，可点击运行一次"); request.clear(); }
    else if(!outputOwned.load()) currentStatus.message=u8"等待触发";
    rearm=true;
}
void simulate(const std::string& id){std::lock_guard<std::mutex> lock(stateMutex);simulationRequest=id;}
std::string ruleDiagnostics(){std::lock_guard<std::mutex> lock(stateMutex);std::ostringstream out;
    if(executor)for(const auto& item:executor->stats()){const auto& v=item.second;out<<item.first<<u8"：启动 "<<v.starts<<u8" 次，完成 "<<v.completed<<u8" 次，中断 "<<v.cancelled<<u8" 次，上次耗时 "<<v.lastElapsedMs<<" ms\n";}
    if(executor)for(const auto& line:executor->log())out<<line<<'\n';
    if(simulator)for(const auto& line:simulator->log())out<<line<<'\n';
    for(const auto& p:programs)for(size_t i=0;i<p.conditions.size();++i){const auto v=evaluate(p,latestSnapshot,int(i));out<<p.name<<u8" 条件 "<<i+1<<u8"："<<(v.known?(v.number?u8"满足":u8"不满足"):v.error)<<'\n';}
    return out.str();
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
    ensureExecutor();
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
        executor->configure(programs,differentDeviceOrProfile);simulator->configure(programs);configureSources(programs,master,cfg->detection_resolution);sourceResolution=cfg->detection_resolution;
        rearm=true; if(differentDeviceOrProfile) request.clear();
    }
    seenConfig=cfg;
    if(sourceResolution!=cfg->detection_resolution){configureSources(programs,master,cfg->detection_resolution);sourceResolution=cfg->detection_resolution;}
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
    if(!master&&simulationRequest.empty()&&!simulator->active()){currentStatus.message=u8"宏编排已关闭";request.clear();return;}
    latestSnapshot=snapshot(*cfg);
    const auto dry=std::exchange(simulationRequest,std::string{});
    if(!dry.empty()){simulator->configure(programs);simulator->run(dry,latestSnapshot,true);}
    simulator->tick(latestSnapshot,true);
    if(!master){currentStatus.message=u8"宏编排已关闭；可使用模拟运行";request.clear();return;}
    std::lock_guard<std::recursive_mutex> output(outputLock);
    if(pressed(stopKey.empty()?"F12":stopKey)){finish(u8"已按停止键，全部宏停止");request.clear();rearm=true;return;}
    const auto manual=std::exchange(request,std::string{});
    if(!manual.empty())executor->run(manual,latestSnapshot);
    executor->tick(latestSnapshot,rearm||editing);rearm=false;
    const auto records=executor->records();currentStatus.running=!records.empty();
    if(!records.empty()){currentStatus.id=records.front().rule;currentStatus.step=records.front().step;currentStatus.message=records.front().name+u8"："+records.front().message;
        for(const auto& p:programs)if(p.id==currentStatus.id)currentStatus.total=int(p.actions.size());}
    else if(!executor->log().empty())currentStatus.message=executor->log().back();
}
} // namespace macros
