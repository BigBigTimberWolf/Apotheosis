#pragma once

#include <algorithm>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>
#include <map>
#include <array>
#include <cmath>

namespace macros {
enum class ActionType { Delay, KeyDown, KeyUp, KeyPress, MouseMove,
    MouseDown, MouseUp, MouseClick, Wheel, PidReset, PidResetX, PidResetY,
    Loop, EndLoop, While, If, Else, EndIf, Break, Continue, WaitCondition,
    Stop, SetVariable, AddVariable, Log, Call, EnableRule, DisableRule, PauseRule, ResumeRule,
    Text, AbsoluteMove, SmoothMove, CurveMove, MoveToTarget, MoveToPrediction,
    LockTarget, UnlockTarget, NextTarget, ClearTarget, AimPart, AimClass, AimPriority,
    Prediction, Smoothing, SpeedLimit, Sound, Vibration, Notification, SwitchProfile,
    ExportConfig, ImportConfig, Parallel, EndParallel, Retry, ForTargets, Switch, Case, EndSwitch,
    Jump };
enum class Mode { Once, Hold, Toggle, Sequence };

// Parameter directives need the aim loop running. Only input injection takes
// exclusive keyboard/mouse ownership; the scheduler's waits stay nonblocking.
inline bool exclusiveOutput(ActionType type) {
    switch (type) {
    case ActionType::KeyDown: case ActionType::KeyUp: case ActionType::KeyPress:
    case ActionType::MouseDown: case ActionType::MouseUp: case ActionType::MouseClick:
    case ActionType::MouseMove: case ActionType::Wheel: case ActionType::Text:
    case ActionType::AbsoluteMove: case ActionType::SmoothMove: case ActionType::CurveMove:
    case ActionType::MoveToTarget: case ActionType::MoveToPrediction: return true;
    default: return false;
    }
}

struct Action {
    ActionType type = ActionType::Delay;
    std::string key = "Key1";
    int a = 100, b = 100;
    int c = 0, d = 0;
    double value = 0;
    std::string text;
    bool operator==(const Action& v) const {
        return std::tie(type,key,a,b,c,d,value,text) == std::tie(v.type,v.key,v.a,v.b,v.c,v.d,v.value,v.text);
    }
};
struct Condition {
    // Parent indexes describe a bounded tree. -1 means implicit root AND.
    std::string metric="target.exists", comparison="==", text;
    double value=1, upper=1;
    int classId=-1, parent=-1;
    std::array<int,4> region{}; // x,y,w,h in the inference image; w/h=0 means whole image.
    bool operator==(const Condition& v) const {
        return std::tie(metric,comparison,text,value,upper,classId,parent,region)==
            std::tie(v.metric,v.comparison,v.text,v.value,v.upper,v.classId,v.parent,v.region);
    }
};
struct Program {
    std::string id, name = u8"新建宏", trigger;
    bool enabled = false, targetOnly = false, heightFilter = false;
    bool blockTrigger = false;
    int minHeightPercent = 0, maxHeightPercent = 100;
    std::vector<int> classes;
    Mode mode = Mode::Once;
    int loopIntervalMs = 20;
    std::vector<Action> actions;
    std::vector<Condition> conditions;
    // Extensible rule settings are persisted verbatim; legacy INIs have none.
    std::map<std::string,std::string> options;
    bool operator==(const Program& v) const {
        return std::tie(id,name,trigger,enabled,blockTrigger,targetOnly,heightFilter,minHeightPercent,
            maxHeightPercent,classes,mode,loopIntervalMs,actions,conditions,options) ==
            std::tie(v.id,v.name,v.trigger,v.enabled,v.blockTrigger,v.targetOnly,v.heightFilter,
                v.minHeightPercent,v.maxHeightPercent,v.classes,v.mode,v.loopIntervalMs,v.actions,v.conditions,v.options);
    }
};
inline constexpr int maxPrograms = 64, maxActions = 512, maxConditions = 128;
inline std::string encodeText(const std::string& text) {
    static constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(text.size()*2);
    for(unsigned char c:text){out+=digits[c>>4];out+=digits[c&15];}return out;
}
inline std::string decodeText(const std::string& text,const std::string& fallback={}) {
    if(text.size()%2)return fallback;std::string out;out.reserve(text.size()/2);
    auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
    for(size_t i=0;i<text.size();i+=2){const int a=digit(text[i]),b=digit(text[i+1]);if(a<0||b<0)return fallback;out+=char(a*16+b);}return out;
}
inline std::string option(const Program& p,const std::string& key,const std::string& fallback={}) {
    const auto it=p.options.find(key); return it==p.options.end()?fallback:it->second;
}
inline int number(const Program& p,const std::string& key,int fallback=0) {
    try { return std::stoi(option(p,key)); } catch(...) { return fallback; }
}
struct Key { std::string id, label; int hid; };
inline const std::vector<Key>& keys() {
    static const std::vector<Key> value = [] {
        std::vector<Key> v;
        for (char c = 'A'; c <= 'Z'; ++c) v.push_back({std::string(1,c),std::string(1,c),4+c-'A'});
        for (int i=0; i<10; ++i) v.push_back({"Key"+std::to_string(i),std::to_string(i),i ? 29+i : 39});
        for (int i=1; i<=12; ++i) v.push_back({"F"+std::to_string(i),"F"+std::to_string(i),57+i});
        const Key special[] = {{"Space",u8"空格",44},{"Enter",u8"回车",40},
            {"Escape","Esc",41},{"Tab","Tab",43},{"Backspace",u8"退格",42},
            {"LeftShift","左 Shift",225},{"RightShift","右 Shift",229},
            {"LeftControl","左 Ctrl",224},{"RightControl","右 Ctrl",228},
            {"LeftAlt","左 Alt",226},{"RightAlt","右 Alt",230},
            {"UpArrow",u8"↑",82},{"DownArrow",u8"↓",81},{"LeftArrow",u8"←",80},
            {"RightArrow",u8"→",79},{"Home","Home",74},{"End","End",77},
            {"PageUp","Page Up",75},{"PageDown","Page Down",78},{"Delete","Delete",76}};
        v.insert(v.end(),std::begin(special),std::end(special));
        for(int i=13;i<=24;++i) v.push_back({"F"+std::to_string(i),"F"+std::to_string(i),104+i-13});
        const Key extra[]={
            {"CapsLock","Caps Lock",57},{"PrintScreen","Print Screen",70},{"ScrollLock","Scroll Lock",71},
            {"Pause","Pause",72},{"Ins","Insert",73},{"NumLock","Num Lock",83},
            {"Minus","-",45},{"Equal","=",46},{"LeftBracket","[",47},{"RightBracket","]",48},
            {"Backslash","\\",49},{"Semicolon",";",51},{"Quote","'",52},{"Backquote","`",53},
            {"Comma",",",54},{"Period",".",55},{"Slash","/",56},
            {"LeftWindowsKey","左 Win",227},{"RightWindowsKey","右 Win",231},{"Application","菜单键",101},
            {"Divide","数字区 /",84},{"Multiply","数字区 *",85},{"Subtract","数字区 -",86},
            {"Add","数字区 +",87},{"NumpadEnter","数字区回车",88},{"Decimal","数字区 .",99}};
        v.insert(v.end(),std::begin(extra),std::end(extra));
        for(int i=0;i<=9;++i) v.push_back({"NumpadKey"+std::to_string(i),u8"数字区 "+std::to_string(i),i?88+i:98});
        const Key media[]={{"VolumeMute",u8"静音（本机）",0x100AD},{"VolumeDown",u8"降低音量（本机）",0x100AE},{"VolumeUp",u8"提高音量（本机）",0x100AF},
            {"NextTrack",u8"下一曲（本机）",0x100B0},{"PreviousTrack",u8"上一曲（本机）",0x100B1},{"StopMedia",u8"停止媒体（本机）",0x100B2},{"PlayMedia",u8"播放 / 暂停（本机）",0x100B3},
            {"BrowserBack",u8"浏览器后退（本机）",0x100A6},{"BrowserForward",u8"浏览器前进（本机）",0x100A7},{"BrowserRefresh",u8"浏览器刷新（本机）",0x100A8},{"StartMailKey",u8"邮件（本机）",0x100B4}};
        v.insert(v.end(),std::begin(media),std::end(media));
        return v;
    }();
    return value;
}
inline int hidKey(const std::string& id) {
    if(id.rfind("VK:",0)==0){try{size_t used=0;const int vk=std::stoi(id.substr(3),&used,0);if(used==id.size()-3&&vk>0&&vk<256)return 0x10000|vk;}catch(...){}return 0;}
    for (const auto& k : keys()) if (k.id == id) return k.hid;
    return 0;
}

// 滚轮没有 HID 按键码：它在按键列表里用固定 id 表示，输出按“格”计数。
// 需要输出滚轮的地方（自动爆闪、宏动作）发送 WheelUp=+1 / WheelDown=-1 格，
// 读取滚轮只能靠本机钩子（盒子不上报物理滚轮）。
struct WheelKey { const char* id; const char* label; };
inline const std::vector<WheelKey>& wheelKeys() {
    static const std::vector<WheelKey> value = {
        {"WheelUp", u8"滚轮 · 上"}, {"WheelDown", u8"滚轮 · 下"}};
    return value;
}
inline bool wheelKeyId(const std::string& id) {
    for (const auto& k : wheelKeys()) if (id == k.id) return true;
    return false;
}
// 输出格数：上滚 +1、下滚 -1，其它按键为 0。
inline int wheelNotches(const std::string& id) {
    if (id == "WheelUp") return 1;
    if (id == "WheelDown") return -1;
    return 0;
}
inline void normalize(Program& p) {
    auto clean = [](std::string& s) { s.erase(std::remove_if(s.begin(),s.end(),
        [](char c){ return c=='\r'||c=='\n'; }),s.end()); if(s.size()>160) s.resize(160); };
    clean(p.id); clean(p.name); clean(p.trigger);
    p.mode = static_cast<Mode>(std::clamp(static_cast<int>(p.mode),0,3));
    p.loopIntervalMs = std::clamp(p.loopIntervalMs,1,60000);
    p.minHeightPercent = std::clamp(p.minHeightPercent,0,100);
    p.maxHeightPercent = std::clamp(p.maxHeightPercent,p.minHeightPercent,100);
    if(p.actions.size()>maxActions) p.actions.resize(maxActions);
    if(p.conditions.size()>maxConditions) p.conditions.resize(maxConditions);
    if(p.options.size()>64) p.options.clear();
    auto line=[](std::string& s) { s.erase(std::remove_if(s.begin(),s.end(),[](char c){return c=='\r'||c=='\n';}),s.end()); if(s.size()>4096)s.resize(4096); };
    for(auto& entry:p.options) line(entry.second);
    for(auto& c:p.conditions) { line(c.metric);line(c.comparison);line(c.text); if(!std::isfinite(c.value))c.value=0;if(!std::isfinite(c.upper))c.upper=0; }
    for(auto& a:p.actions) {
        clean(a.key);
        line(a.text); if(!std::isfinite(a.value))a.value=0;
        // Unknown imported actions are deliberately retained and rejected at execution.
        if(a.type==ActionType::Delay) {
            a.a=std::clamp(a.a,0,60000); a.b=std::clamp(a.b,a.a,60000);
        } else if(a.type==ActionType::MouseMove) {
            a.a=std::clamp(a.a,-32767,32767); a.b=std::clamp(a.b,-32767,32767);
        } else if(a.type==ActionType::Wheel) a.a=std::clamp(a.a,-127,127);
        else if(a.type==ActionType::KeyPress||a.type==ActionType::MouseClick)
            a.b=std::clamp(a.b,1,2000);
    }
}
} // namespace macros
