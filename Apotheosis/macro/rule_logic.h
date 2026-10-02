#pragma once
#include "rule_schema.h"
#include <set>
#include <deque>
#include <sstream>
#include <limits>

namespace macros {
struct Target {
    int id=-1,classId=-1;
    double x=0,y=0,width=0,height=0,confidence=0,vx=0,vy=0;
};
struct Value {
    bool known=false;
    double number=0;
    std::string text,error;
    static Value numeric(double n) { return {std::isfinite(n),n,{},{}}; }
    static Value string(std::string s) { return {true,0,std::move(s),{}}; }
};
struct RuleSnapshot {
    int64_t now=0;
    int width=0,height=0,targetId=-1;
    std::vector<Target> targets;
    std::map<std::string,Value> values;
    std::set<std::string> keys;
    std::map<std::string,double> variables;
};
inline std::vector<std::string> split(const std::string& s,char separator) {
    std::vector<std::string> v;std::istringstream in(s);std::string part;
    while(std::getline(in,part,separator)) { const auto a=part.find_first_not_of(" \t"),b=part.find_last_not_of(" \t");
        if(a!=std::string::npos)v.push_back(part.substr(a,b-a+1)); } return v;
}
inline bool keyDown(const RuleSnapshot& s,const std::string& chord) {
    const auto keys=split(chord,'+');if(keys.empty())return false;
    for(const auto& key:keys)if(!s.keys.count(key))return false;return true;
}
inline std::string sensorKey(const Condition& c) {
    std::ostringstream o;o<<c.metric<<'|'<<c.text<<'|'<<c.value<<'|'<<c.upper;
    for(int r:c.region)o<<'|'<<r;return o.str();
}
inline Value metric(const Condition& c,const RuleSnapshot& s) {
    if(c.metric=="window.exists"||c.metric=="process.exists") {
        const auto ready=s.values.find(sensorKey(c));if(ready!=s.values.end())return ready->second;
        auto it=s.values.find(c.metric=="window.exists"?"window.list":"process.list");if(it==s.values.end())return {};
        bool exists=false;for(const auto& item:split(it->second.text,'\n'))exists|=c.metric=="window.exists"?item.find(c.text)!=std::string::npos:item==c.text;
        return Value::numeric(exists);
    }
    if(c.metric=="pad.axis") {auto it=s.values.find(c.text);return it==s.values.end()?Value{false,0,{},u8"手柄未连接或轴名称无效"}:it->second;}
    if(c.metric=="key")return Value::numeric(keyDown(s,c.text));
    if(c.metric=="variable") { auto it=s.variables.find(c.text);return Value::numeric(it==s.variables.end()?0:it->second); }
    if(c.metric=="probability") { auto it=s.values.find("random.percent");return it==s.values.end()?Value{}:it->second; }
    if(c.metric.rfind("target.",0)==0) {
        std::vector<const Target*> targets;
        for(const auto& t:s.targets)if(c.classId<0 || t.classId==c.classId)targets.push_back(&t);
        if(c.metric=="target.exists")return Value::numeric(!targets.empty());
        if(c.metric=="target.count")return Value::numeric(double(targets.size()));
        if(c.metric=="target.id")return Value::numeric(s.targetId);
        if(c.metric=="target.visible")return Value::numeric(!targets.empty());
        if(c.metric=="target.lock_ms" || c.metric=="target.occluded" || c.metric=="target.fov" || c.metric=="target.locked") {
            auto it=s.values.find(c.metric);return it==s.values.end()?Value{}:it->second;
        }
        if(targets.empty())return {false,0,{},u8"当前没有指定类别的目标"};
        const Target* chosen=targets.front();
        for(auto* t:targets)if(t->id==s.targetId)chosen=t;
        if(c.metric=="target.class")return Value::numeric(chosen->classId);
        if(c.metric=="target.x")return Value::numeric(chosen->x);
        if(c.metric=="target.y")return Value::numeric(chosen->y);
        if(c.metric=="target.ratio")return Value::numeric(chosen->width/std::max(1.,chosen->height));
        if(c.metric=="target.distance")return Value::numeric(std::hypot(chosen->x-s.width*.5,chosen->y-s.height*.5));
        if(c.metric=="target.angle")return Value::numeric(std::atan2(chosen->y-s.height*.5,chosen->x-s.width*.5)*180/3.141592653589793);
        if(c.metric=="target.speed")return Value::numeric(std::hypot(chosen->vx,chosen->vy));
        if(c.metric=="target.direction")return Value::numeric(std::atan2(chosen->vy,chosen->vx)*180/3.141592653589793);
        if(c.metric=="target.region")return Value::numeric(chosen->x>=c.region[0] && chosen->y>=c.region[1] &&
            chosen->x<c.region[0]+c.region[2] && chosen->y<c.region[1]+c.region[3]);
        bool supported=false;for(const auto& field:metrics())supported|=c.metric==field.id;
        if(!supported)return {false,0,{},u8"未知目标条件："+c.metric};
        double n=0; const bool minimum=c.metric.find(".min_")!=std::string::npos;
        if(minimum)n=std::numeric_limits<double>::infinity();
        for(const auto* t:targets) {
            const double v=c.metric=="target.confidence"?t->confidence:c.metric=="target.area"?t->width*t->height:
                c.metric.find("height")!=std::string::npos?t->height:t->width;
            if(c.metric.find(".mean_")!=std::string::npos)n+=v/targets.size();
            else n=minimum?std::min(n,v):std::max(n,v);
        }
        return Value::numeric(n);
    }
    auto it=s.values.find(sensorKey(c));if(it!=s.values.end())return it->second;
    if(c.metric.rfind("image.",0)==0||c.metric.rfind("pixel",0)==0||c.metric=="ocr.text")
        return {false,0,{},u8"此区域 / 模板的识别结果尚未就绪"};
    it=s.values.find(c.metric);return it==s.values.end()?Value{false,0,{},u8"数据尚未就绪："+c.metric}:it->second;
}
inline bool compare(double n,const std::string& op,double a,double b) {
    if(op=="==")return n==a;if(op=="!=")return n!=a;if(op==">")return n>a;if(op==">=")return n>=a;
    if(op=="<")return n<a;if(op=="<=")return n<=a;if(op=="range")return n>=a && n<=b;return false;
}
inline Value evaluate(const Program& p,const RuleSnapshot& s,int parent=-1,int depth=0) {
    if(depth>16)return {false,0,{},u8"条件组嵌套超过 16 层"};
    std::string kind="all";double needed=1;
    if(parent>=0) {
        if(parent>=int(p.conditions.size()))return {false,0,{},u8"条件编号不存在"};
        const auto& c=p.conditions[parent];kind=c.metric;needed=c.value;
        if(!groupMetric(kind)) {
            auto v=metric(c,s);if(!v.known)return v;
            bool ok;
            if(c.comparison=="contains" || c.comparison=="text_eq" || c.comparison=="text_ne") {
                ok=c.comparison=="contains"?v.text.find(c.text)!=std::string::npos:
                    c.comparison=="text_eq"?v.text==c.text:v.text!=c.text;
            } else ok=compare(v.number,c.comparison,c.value,c.upper);
            if(c.metric=="probability")ok=v.number<c.value;
            if(c.metric=="target.max_size") {
                auto height=c;height.metric="target.max_height";
                const auto h=metric(height,s);ok=ok && h.known && compare(h.number,c.comparison,c.upper,c.upper);
            }
            return Value::numeric(ok);
        }
    }
    int total=0,yes=0,unknown=0;std::string error;
    for(size_t i=0;i<p.conditions.size();++i)if(p.conditions[i].parent==parent) {
        ++total;const auto v=evaluate(p,s,int(i),depth+1);if(!v.known) {++unknown;error=v.error;} else yes+=v.number!=0;
    }
    // Unknown must never become true through NOT/!=/an empty missing target.
    if(kind=="all" && yes+unknown<total)return Value::numeric(0);
    if(kind=="any" && yes)return Value::numeric(1);
    if(kind=="at_least" && yes>=needed)return Value::numeric(1);
    if(unknown)return {false,0,{},error};
    return Value::numeric(kind=="all"?yes==total:kind=="any"?yes>0:kind=="none"?yes==0:kind=="xor"?yes%2==1:yes>=needed);
}
inline bool conditionsMatch(const Program& p,const RuleSnapshot& s) {
    const auto v=evaluate(p,s);if(!v.known || !v.number)return false;
    if(p.targetOnly) {
        bool found=false;for(const auto& t:s.targets) {
            if(!p.classes.empty() && std::find(p.classes.begin(),p.classes.end(),t.classId)==p.classes.end())continue;
            const double h=100*t.height/std::max(1,s.height);
            if(!p.heightFilter || (h>=p.minHeightPercent && h<=p.maxHeightPercent))found=true;
        }if(!found)return false;
    }
    auto contains=[&](const char* field,const char* opt) {const auto wanted=option(p,opt);if(wanted.empty())return true;
        auto it=s.values.find(field);return it!=s.values.end() && it->second.known && it->second.text.find(wanted)!=std::string::npos;};
    return contains("window.title","scope_window") && contains("process.name","scope_process") &&
        (!number(p,"scope_width") || number(p,"scope_width")==s.width) &&
        (!number(p,"scope_height") || number(p,"scope_height")==s.height);
}
inline std::vector<Condition> eventVisionConditions(const Program& p,bool image) {
    std::vector<Condition> selected;const auto pattern=option(p,"pattern");
    for(const auto& c:p.conditions)if((image?(c.metric=="image.hit"||c.metric=="image.score"):c.metric=="ocr.text")&&(pattern.empty()||c.text==pattern))selected.push_back(c);
    if(selected.empty()){Condition c;c.metric=image?"image.hit":"ocr.text";c.text=pattern;if(image)c.upper=.9;selected.push_back(c);}return selected;
}
inline Value eventVisionHit(const Program& p,const RuleSnapshot& s,bool image) {
    bool unknown=false;std::string error;
    for(const auto& c:eventVisionConditions(p,image)){const auto value=metric(c,s);if(!value.known){unknown=true;error=value.error;continue;}
        const bool hit=image?(c.metric=="image.hit"?value.number!=0:compare(value.number,c.comparison,c.value,c.upper)):
            (c.text.empty()?!value.text.empty():value.text.find(c.text)!=std::string::npos);
        if(hit)return Value::numeric(1);
    }return unknown?Value{false,0,{},error}:Value::numeric(0);
}
struct RuleGate {
    bool startupPending=true,profilePending=false;
    bool ready=false,previousDown=false,previousCondition=false,everTarget=false,longFired=false,keyLocked=false,pending=false;
    int previousTarget=-1,lockedTarget=-1,clicks=0,sequenceIndex=0,starts=0;
    int64_t pressedAt=0,lastClick=0,lastStart=-1000000000,enabledAt=0,conditionSince=0;
    RuleSnapshot previous;
    std::deque<int64_t> recent;
    void started(const Program& p,const RuleSnapshot& s) {
        ++starts;lastStart=s.now;recent.push_back(s.now);keyLocked=number(p,"key_lock")!=0;
        if(number(p,"target_lock"))lockedTarget=s.targetId;
    }
    bool poll(const Program& p,const RuleSnapshot& s,bool suppressed=false) {
        const bool down=keyDown(s,p.trigger),condition=conditionsMatch(p,s),found=!s.targets.empty();
        auto bit=[](const RuleSnapshot& a,const std::string& key){auto i=a.values.find(key);return i!=a.values.end()&&i->second.known&&i->second.number!=0;};
        auto changed=[&](const std::string& key){auto a=s.values.find(key);auto b=previous.values.find(key);
            return a!=s.values.end() && b!=previous.values.end() && a->second.known&&b->second.known &&
                (a->second.number!=b->second.number || a->second.text!=b->second.text);};
        if(!ready || suppressed) {
            if(!ready){enabledAt=s.now;conditionSince=s.now;}
            ready=true;previous=s;previousDown=down;previousCondition=condition;previousTarget=s.targetId;
            if(down)pressedAt=s.now;if(option(p,"event")!="target_first")everTarget|=found;pending=false;return false;
        }
        bool anyDown=false;for(const auto& k:split(p.trigger,'+'))anyDown|=s.keys.count(k)!=0;
        if(!anyDown)keyLocked=false;
        if(!found || lockedTarget!=s.targetId)lockedTarget=-1;
        const bool press=down&&!previousDown,release=!down&&previousDown;
        if(press) {pressedAt=s.now;longFired=false;clicks=s.now-lastClick<=number(p,"gesture_ms",350)?clicks+1:1;lastClick=s.now;}
        const auto event=option(p,"event","key_down");
        bool fire=false;
        if(event=="key_down")fire=press;
        else if(event=="key_up")fire=release;
        else if(event=="key_held")fire=down;
        else if(event=="click")fire=release;
        else if(event=="double"||event=="triple") {fire=press&&clicks==(event=="double"?2:3);if(fire)clicks=0;}
        else if(event=="long") {fire=down&&!longFired&&s.now-pressedAt>=number(p,"gesture_ms",350);if(fire)longFired=true;}
        else if(event=="short")fire=release&&s.now-pressedAt<number(p,"gesture_ms",350);
        else if(event=="sequence") {
            const auto keys=split(p.trigger,'>');if(s.now-lastClick>number(p,"gesture_ms",350))sequenceIndex=0;
            if(!keys.empty())for(size_t i=0;i<keys.size();++i)if(keyDown(s,keys[i])&&!keyDown(previous,keys[i])) {
                if(int(i)==sequenceIndex)++sequenceIndex;else sequenceIndex=i==0?1:0;lastClick=s.now;
                if(sequenceIndex==int(keys.size())) {fire=true;sequenceIndex=0;}break;
            }
        }
        else if(event=="target_found")fire=found&&previous.targets.empty();
        else if(event=="target_lost")fire=!found&&!previous.targets.empty();
        else if(event=="target_first")fire=found&&!everTarget;
        else if(event=="target_reacquire")fire=found&&previous.targets.empty()&&everTarget;
        else if(event=="target_held")fire=found;
        else if(event=="target_switch")fire=s.targetId>=0&&previousTarget>=0&&s.targetId!=previousTarget;
        else if(event=="target_lock")fire=bit(s,"target.locked")&&!bit(previous,"target.locked");
        else if(event=="target_unlock")fire=!bit(s,"target.locked")&&bit(previous,"target.locked");
        else if(event=="fov_enter")fire=bit(s,"target.fov")&&!bit(previous,"target.fov");
        else if(event=="fov_leave")fire=!bit(s,"target.fov")&&bit(previous,"target.fov");
        else if(event=="conditions")fire=condition&&(number(p,"level")||!previousCondition);
        else if(event=="timer")fire=s.now-lastStart>=std::max(1,number(p,"timer_ms",1000))&&s.now-enabledAt>=std::max(1,number(p,"timer_ms",1000));
        else if(event=="cooldown")fire=lastStart>0&&s.now-lastStart>=std::max(1,number(p,"cooldown_ms"))&&previous.now-lastStart<std::max(1,number(p,"cooldown_ms"));
        else if(event=="metric_change") {const auto name=option(p,"event_metric","target.count");
            if(name.rfind("variable:",0)==0){const auto key=name.substr(9);auto value=[&](const RuleSnapshot& x){auto it=x.variables.find(key);return it==x.variables.end()?0:it->second;};fire=value(s)!=value(previous);}
            else {Condition c;c.metric=name;auto a=metric(c,s),b=metric(c,previous);fire=a.known&&b.known&&(a.number!=b.number||a.text!=b.text);}}
        else if(event=="profile_change")fire=profilePending||changed("profile");
        else if(event=="resolution_change")fire=s.width!=previous.width||s.height!=previous.height;
        else {
            std::string key;bool rising=true;
            if(event=="aim_start"||event=="aim_stop") {key="aim.active";rising=event=="aim_start";}
            if(event=="trigger_start"||event=="trigger_stop") {key="trigger.active";rising=event=="trigger_start";}
            if(event=="color_found"||event=="color_lost") {key="color.hit";rising=event=="color_found";}
            if(event=="image_found"||event=="image_lost") {key="image.hit";rising=event=="image_found";}
            if(event=="ocr_found"||event=="ocr_lost") {key="ocr.hit";rising=event=="ocr_found";}
            if(event=="window_enter"||event=="window_leave") {key="window.match";rising=event=="window_enter";}
            if(event=="window_found")key="window.exists";
            if(event=="process_found")key="process.exists";
            if(event=="rule_enable"||event=="rule_disable") {key="rule.enabled:"+option(p,"pattern",p.id);rising=event=="rule_enable";}
            if(!key.empty()){
                auto a=s.values.find(key);auto b=previous.values.find(key);
                if(a!=s.values.end()&&a->second.known&&(rising||(b!=previous.values.end()&&b->second.known)))
                    fire=rising?bit(s,key)&&!bit(previous,key):!bit(s,key)&&bit(previous,key);
            }
            if(event=="rule_enable"&&startupPending&&(option(p,"pattern").empty()||option(p,"pattern")==p.id))fire=true;
        }
        startupPending=false;profilePending=false;
        everTarget|=found;
        if(condition&&!previousCondition)conditionSince=s.now;
        if(!condition)pending=false;
        if(fire&&condition)pending=true;
        fire=pending&&condition&&s.now-conditionSince>=std::max(0,number(p,"debounce_ms"));
        if(fire)pending=false;
        const int64_t window=std::max(1,number(p,"window_ms",1000));
        while(!recent.empty()&&s.now-recent.front()>=window)recent.pop_front();
        if(keyLocked || (number(p,"target_lock")&&s.targetId>=0&&lockedTarget==s.targetId) ||
            (number(p,"max_count")>0&&starts>=number(p,"max_count")) ||
            (number(p,"window_count")>0&&int(recent.size())>=number(p,"window_count")) ||
            s.now-lastStart<std::max(number(p,"cooldown_ms"),event=="key_held"||event=="target_held"?p.loopIntervalMs:0))fire=false;
        previous=s;previousDown=down;previousCondition=condition;previousTarget=s.targetId;
        return fire;
    }
};

struct FlowMap {
    std::vector<int> end,begin,otherwise;
    std::string error;
};
inline FlowMap compileFlow(const Program& p) {
    FlowMap f;const int n=int(p.actions.size());f.end.assign(n,-1);f.begin.assign(n,-1);f.otherwise.assign(n,-1);
    if(number(p,"cancel_condition")<0||number(p,"cancel_condition")>int(p.conditions.size()))f.error=u8"中断条件编号不存在";
    std::vector<int> stack;
    for(int i=0;i<n;++i) {
        const auto type=p.actions[i].type;
        const auto& action=p.actions[i];
        if(type==ActionType::Jump&&(action.a<1||action.a>n)){f.error=u8"跳转步骤不在当前流程内";break;}
        const bool conditional=type==ActionType::If||type==ActionType::While||type==ActionType::WaitCondition||type==ActionType::Retry||type==ActionType::Jump;
        const int conditionIndex=type==ActionType::Jump?action.b:action.a;
        if(conditional&&(conditionIndex<0||conditionIndex>int(p.conditions.size()))){f.error=u8"动作引用的条件不存在：步骤 "+std::to_string(i+1);break;}
        if(int(type)<0 || int(type)>=int(actionLabels().size())) {f.error=u8"未知动作：步骤 "+std::to_string(i+1);break;}
        const bool open=type==ActionType::Loop||type==ActionType::While||type==ActionType::If||type==ActionType::Parallel||
            type==ActionType::Retry||type==ActionType::ForTargets||type==ActionType::Switch;
        if(open) {stack.push_back(i);if(stack.size()>16){f.error=u8"流程嵌套超过 16 层";break;}}
        else if(type==ActionType::Else||type==ActionType::Case) {
            if(stack.empty() || (type==ActionType::Else?p.actions[stack.back()].type!=ActionType::If:
                p.actions[stack.back()].type!=ActionType::Switch)) {f.error=u8"分支缺少对应开始：步骤 "+std::to_string(i+1);break;}
            if(type==ActionType::Else) {if(f.otherwise[stack.back()]>=0){f.error=u8"同一个分支有多个否则";break;}f.otherwise[stack.back()]=i;}
            f.begin[i]=stack.back();
        } else if(type==ActionType::EndLoop||type==ActionType::EndIf||type==ActionType::EndParallel||type==ActionType::EndSwitch) {
            if(stack.empty()) {f.error=u8"结束动作没有开始：步骤 "+std::to_string(i+1);break;}
            const auto start=stack.back();const auto t=p.actions[start].type;
            const bool match=type==ActionType::EndIf?t==ActionType::If:type==ActionType::EndParallel?t==ActionType::Parallel:
                type==ActionType::EndSwitch?t==ActionType::Switch:t==ActionType::Loop||t==ActionType::While||t==ActionType::Retry||t==ActionType::ForTargets;
            if(!match){f.error=u8"嵌套流程结束类型不匹配：步骤 "+std::to_string(i+1);break;}
            stack.pop_back();f.end[start]=i;f.begin[i]=start;
            for(int j=start+1;j<i;++j)if(f.begin[j]==start)f.end[j]=i;
        } else if(type==ActionType::Break||type==ActionType::Continue) {
            int loop=-1;for(auto it=stack.rbegin();it!=stack.rend();++it) {
                const auto t=p.actions[*it].type;if(t==ActionType::Loop||t==ActionType::While||t==ActionType::ForTargets||t==ActionType::Retry){loop=*it;break;}}
            if(loop<0){f.error=u8"跳出 / 继续必须放在循环内";break;}f.begin[i]=loop;
        }
    }
    if(f.error.empty()&&!stack.empty())f.error=u8"流程尚未闭合：步骤 "+std::to_string(stack.back()+1);
    for(size_t i=0;i<p.conditions.size() && f.error.empty();++i) {
        const auto& c=p.conditions[i];if(c.parent>=int(i)||c.parent< -2 || (c.parent>=0&&!groupMetric(p.conditions[c.parent].metric)))
            f.error=u8"条件父组必须是前面的条件组：条件 "+std::to_string(i+1);
        bool known=false;for(const auto& field:metrics())known|=c.metric==field.id;
        if(!known)f.error=u8"未知条件类型："+c.metric;
    }
    return f;
}
inline std::string describeRule(const Program& p) {
    std::ostringstream o;o<<u8"当「"<<label(events(),option(p,"event","key_down"))<<u8"」";
    if(!p.trigger.empty())o<<u8"（"<<p.trigger<<u8"）";
    if(p.targetOnly)o<<u8"，且存在符合原类别 / 框高筛选的目标";
    for(size_t i=0;i<p.conditions.size();++i) {const auto& c=p.conditions[i];o<<u8"；条件 "<<i+1<<u8"：";
        if(c.parent==-2)o<<u8"仅供动作判断，不限制启动，";
        else if(c.parent>=0)o<<u8"属于组 "<<c.parent+1<<u8"，";
        o<<label(metrics(),c.metric);if(!c.text.empty())o<<u8"（"<<c.text<<u8"）";
        if(c.classId>=0)o<<u8"，类别 "<<c.classId;
        if(!groupMetric(c.metric))o<<' '<<c.comparison<<' '<<c.value;
        if(c.comparison=="range"||c.metric=="target.max_size")o<<u8"，上限 / 高度 "<<c.upper;
    }
    o<<u8"。满足后执行：";
    for(size_t i=0;i<p.actions.size();++i) {const auto& a=p.actions[i];if(i)o<<u8" → ";
        if(int(a.type)>=0&&int(a.type)<int(actionLabels().size()))o<<actionLabels()[int(a.type)];
        if(a.type==ActionType::Delay)o<<' '<<a.a<<u8"～"<<a.b<<"ms";
        else if(a.type==ActionType::KeyDown||a.type==ActionType::KeyUp||a.type==ActionType::KeyPress)o<<' '<<a.key;
        else if(a.type==ActionType::MouseDown||a.type==ActionType::MouseUp||a.type==ActionType::MouseClick)o<<u8" 按钮 "<<a.a;
        if(a.type==ActionType::KeyPress||a.type==ActionType::MouseClick)o<<u8"，保持 "<<a.b<<u8"ms 后松开";
        if(a.type==ActionType::Loop)o<<' '<<a.a<<u8" 次";
        if(int(a.type)>11&&a.type!=ActionType::Loop){const auto fields=actionFields(a.type);
            if(*fields.a)o<<u8"（"<<fields.a<<'='<<a.a<<u8"）";
            if(*fields.b)o<<u8"（"<<fields.b<<'='<<a.b<<u8"）";
            if(*fields.c)o<<u8"（"<<fields.c<<'='<<a.c<<u8"）";
            if(*fields.d)o<<u8"（"<<fields.d<<'='<<a.d<<u8"）";
            if(*fields.value)o<<u8"（"<<fields.value<<'='<<a.value<<u8"）";
            if(*fields.text)o<<u8"「"<<a.text<<u8"」";}
    }
    o<<u8"。执行期间不允许本规则重入；结束 / 取消时释放本规则持有的输入。";
    o<<(number(p,"level")?u8"条件持续成立可重复触发。":u8"条件满足事件默认只在假变真时触发一次。");
    if(number(p,"cooldown_ms"))o<<u8"冷却 "<<number(p,"cooldown_ms")<<"ms。";
    if(number(p,"target_lock"))o<<u8"同一目标只启动一次，丢失或切换后解锁。";
    if(number(p,"key_lock"))o<<u8"按住期间只启动一次，松开后解锁。";
    if(number(p,"cancel_conditions",1))o<<u8"条件失效取消。";
    if(number(p,"cancel_key"))o<<u8"触发键松开取消。";
    if(number(p,"cancel_target"))o<<u8"原目标丢失或切换取消。";
    if(number(p,"timeout_ms",60000))o<<u8"最长运行 "<<number(p,"timeout_ms",60000)<<"ms。";
    if(!option(p,"mutex").empty())o<<u8"与「"<<option(p,"mutex")<<u8"」组内规则互斥。";
    if(number(p,"parallel"))o<<u8"允许与同样开启并行的其他规则同时执行。";
    if(number(p,"preempt"))o<<u8"可抢占更低优先级规则；本规则优先级 "<<number(p,"priority")<<u8"。";
    const auto f=compileFlow(p);if(!f.error.empty())o<<u8"【无法执行："<<f.error<<u8"】";
    return o.str();
}
}
