#pragma once

#include <algorithm>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

namespace macros {
enum class ActionType { Delay, KeyDown, KeyUp, KeyPress, MouseMove,
    MouseDown, MouseUp, MouseClick, Wheel, PidReset, PidResetX, PidResetY };
enum class Mode { Once, Hold, Toggle, Sequence };

struct Action {
    ActionType type = ActionType::Delay;
    std::string key = "Key1";
    int a = 100, b = 100;
    bool operator==(const Action& v) const {
        return std::tie(type, key, a, b) == std::tie(v.type, v.key, v.a, v.b);
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
    bool operator==(const Program& v) const {
        return std::tie(id,name,trigger,enabled,blockTrigger,targetOnly,heightFilter,minHeightPercent,
            maxHeightPercent,classes,mode,loopIntervalMs,actions) ==
            std::tie(v.id,v.name,v.trigger,v.enabled,v.blockTrigger,v.targetOnly,v.heightFilter,
                v.minHeightPercent,v.maxHeightPercent,v.classes,v.mode,v.loopIntervalMs,v.actions);
    }
};
inline constexpr int maxPrograms = 64, maxActions = 256;
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
        return v;
    }();
    return value;
}
inline int hidKey(const std::string& id) {
    for (const auto& k : keys()) if (k.id == id) return k.hid;
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
    for(auto& a:p.actions) {
        clean(a.key);
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
