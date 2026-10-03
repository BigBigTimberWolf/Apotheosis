#pragma once
#include "rule_logic.h"
#include <functional>
#include <memory>
#include <future>
#include <atomic>

namespace macros {
struct RuleHost {
    // Commands are already timed by the scheduler. No callback may sleep.
    std::function<bool(const Action&,const RuleSnapshot&,std::string&)> action;
    std::function<bool(bool,int,bool)> button;
    std::function<bool()> acquire;
    std::function<void(bool)> release;
    // A valid future suspends this flow until the UI operation completes.
    std::function<std::shared_future<std::string>(const Action&,std::shared_ptr<std::atomic<bool>>)> asyncAction;
    // Explain transport/capability failures instead of reporting a silent no-op.
    std::function<std::string(bool,int)> inputError;
    // Same monotonic millisecond domain as snapshots. Input transports may
    // block; requested hold/delay durations start after a successful send.
    std::function<int64_t()> nowMs;
};
struct RunRecord { uint64_t uid=0;std::string rule,name,message;int step=0;bool dry=false; };
struct RuleStats {uint64_t starts=0,completed=0,cancelled=0,steps=0;int64_t lastElapsedMs=0,totalElapsedMs=0;};
class RuleExecutor {
public:
    explicit RuleExecutor(RuleHost host):host_(std::move(host)){}
    void configure(std::vector<Program> p,bool profileChanged=false) { stop();programs_=std::move(p);gates_.clear();disabled_.clear();variables_.clear();stats_.clear();for(const auto& rule:programs_)gates_[rule.id].profilePending=profileChanged; }
    bool active() const {return !jobs_.empty();}
    std::vector<RunRecord> records() const {
        std::vector<RunRecord> result;
        for(const auto& j:jobs_)result.push_back({j->uid,j->program.id,j->program.name,j->message,int(j->pc+1),j->dry});
        return result;
    }
    const std::deque<std::string>& log() const {return log_;}
    const std::map<std::string,double>& variables() const {return variables_;}
    const std::map<std::string,RuleStats>& stats() const {return stats_;}
    void report(const std::string& text){note(text);}
    void stop() { for(auto& j:jobs_)if(!j->done)end(*j,u8"全部停止");cleanup(); }
    bool run(const std::string& id,const RuleSnapshot& s,bool dry=false) {
        for(const auto& p:programs_)if(p.id==id) {auto context=s;context.variables=variables_;context.values["cooldown.ready"]=Value::numeric(s.now-gates_[p.id].lastStart>=number(p,"cooldown_ms"));const bool ok=start(p,context,dry,0);if(ok)jobs_.back()->manual=true;return ok;}return false;
    }
    void tick(RuleSnapshot s,bool suppress=false) {
        now_=s.now;
        s.variables=variables_;
        for(const auto& p:programs_)s.values["rule.enabled:"+p.id]=Value::numeric(p.enabled&&!disabled_.count(p.id));
        std::vector<const Program*> ranked;
        for(const auto& p:programs_)ranked.push_back(&p);
        std::stable_sort(ranked.begin(),ranked.end(),[](const Program* a,const Program* b){return number(*a,"priority")>number(*b,"priority");});
        for(const auto* p:ranked) {
            if(!p->enabled || disabled_.count(p->id))continue;
            auto& gate=gates_[p->id];
            auto context=s;context.values["cooldown.ready"]=Value::numeric(s.now-gate.lastStart>=number(*p,"cooldown_ms"));
            const auto title=context.values.find("window.title");
            context.values["window.match"]=Value::numeric(title!=context.values.end()&&title->second.text.find(option(*p,"pattern"))!=std::string::npos);
            const auto event=option(*p,"event");
            if(event=="window_found"||event=="process_found") {Condition c;c.metric=event=="window_found"?"window.exists":"process.exists";c.text=option(*p,"pattern");context.values[c.metric]=metric(c,context);}
            if(event=="image_found"||event=="image_lost"||event=="ocr_found"||event=="ocr_lost") {
                const bool image=event.rfind("image",0)==0;context.values[image?"image.hit":"ocr.hit"]=eventVisionHit(*p,context,image);
            }
            if(gate.poll(*p,context,suppress))start(*p,context,false,0);
        }
        const auto pending=jobs_; // Children appended by an action start on the next tick.
        for(const auto& j:pending) {
            if(j->done)continue;
            auto context=s;context.variables=variables_;
            context.values["cooldown.ready"]=Value::numeric(s.now-gates_[j->program.id].lastStart>=number(j->program,"cooldown_ms"));
            context.values["loop.index"]=Value::numeric(j->loopIndex);
            if(j->target>=0)context.targetId=j->target;
            if(j->dry){context=j->initial;context.now=s.now;context.variables=variables_;context.values["loop.index"]=Value::numeric(j->loopIndex);if(j->target>=0)context.targetId=j->target;}
            const int timeout=number(j->program,"timeout_ms",60000);
            const auto extra=number(j->program,"cancel_condition");
            if((timeout>0&&s.now-j->started>=timeout) ||
                (number(j->program,"cancel_conditions",1)&&!conditionsMatch(j->program,context)) ||
                (number(j->program,"cancel_key")&&!keyDown(context,j->program.trigger)) ||
                (number(j->program,"cancel_target")&&j->initial.targetId!=s.targetId) ||
                (extra>0&&truth(j->program,context,extra))) {
                end(*j,u8"超时或中断条件满足");continue;
            }
            if(j->program.mode==Mode::Hold&&!j->manual&&!keyDown(context,j->program.trigger)) {end(*j,u8"触发键松开");continue;}
            if(j->async.valid()){
                if(j->async.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)continue;
                std::string error;try{error=j->async.get();}catch(const std::exception& e){error=u8"系统操作未完成："+std::string(e.what());}
                j->async={};if(!error.empty()){end(*j,error);continue;}
            }
            if(j->pulse && s.now>=j->deadline) {if(!releasePulse(*j)){end(*j,u8"松键失败");continue;}j->pulse=false;}
            if(j->waitChild && alive(j->waitChild))continue;
            j->waitChild=0;
            if(j->paused)continue;
            if(s.now<j->deadline)continue;
            if(j->pulse) {if(!releasePulse(*j)){end(*j,u8"松键失败");continue;}j->pulse=false;}
            for(int budget=0;budget<8 && !j->done && s.now>=j->deadline;++budget) {
                if(j->pc>=j->program.actions.size()) {
                    bool children=false;for(const auto& child:jobs_)children|=!child->done&&child->parent==j->uid;
                    if(children)break;
                    if(!j->manual&&(j->program.mode==Mode::Hold||j->program.mode==Mode::Toggle)) {
                        releaseHeld(*j);j->pc=0;j->counters.clear();j->deadline=s.now+j->program.loopIntervalMs;break;
                    }
                    end(*j,u8"执行完成");break;
                }
                context.variables=variables_;context.values["loop.index"]=Value::numeric(j->loopIndex);context.targetId=j->target>=0?j->target:(j->dry?j->initial.targetId:s.targetId);
                if(!execute(*j,context))break;
                if(j->pulse || j->waitChild || j->moving || j->paused || j->async.valid())break;
            }
        }
        cleanup();
    }
private:
    struct Job {
        Program program;FlowMap flow;RuleSnapshot initial;
        uint64_t uid=0,parent=0,waitChild=0;size_t pc=0;
        int64_t started=0,deadline=0,waitStarted=0,motionStarted=0;
        bool done=false,dry=false,manual=false,pulse=false,paused=false,moving=false,ownsOutput=false;
        int target=-1,loopIndex=0,sentX=0,sentY=0;
        std::vector<std::pair<bool,int>> pulseKeys;
        std::set<std::pair<bool,int>> held;
        std::map<int,int> counters;
        std::map<int,std::vector<int>> targetIds;
        std::map<int,int> previousTarget;
        std::string message;
        std::shared_ptr<std::atomic<bool>> alive=std::make_shared<std::atomic<bool>>(true);
        std::shared_future<std::string> async;
    };
    RuleHost host_;std::vector<Program> programs_;
    std::vector<std::shared_ptr<Job>> jobs_;
    std::map<std::string,RuleGate> gates_;
    std::set<std::string> disabled_;
    std::map<std::pair<bool,int>,int> owners_;
    std::map<std::string,double> variables_;
    std::map<std::string,RuleStats> stats_;
    int64_t now_=0;
    std::deque<std::string> log_;
    std::string lastInputError_;
    uint64_t nextUid_=1;bool acquired_=false,moved_=false;
    void note(const std::string& s) {log_.push_back(s);while(log_.size()>200)log_.pop_front();}
    bool alive(uint64_t id) const {for(const auto& j:jobs_)if(j->uid==id&&!j->done)return true;return false;}
    bool truth(const Program& p,const RuleSnapshot& s,int index) {const auto v=evaluate(p,s,index<=0?-1:index-1);return v.known&&v.number;}
    void end(Job& j,const std::string& reason) {
        if(j.done)return;
        j.alive->store(false);
        auto& stat=stats_[j.program.id];stat.lastElapsedMs=std::max<int64_t>(0,now_-j.started);stat.totalElapsedMs+=stat.lastElapsedMs;
        if(reason==u8"执行完成")++stat.completed;else ++stat.cancelled;
        j.done=true;j.message=reason;note(j.program.name+u8"："+reason);
        for(auto& child:jobs_)if(child->parent==j.uid&&!child->done)end(*child,u8"父流程已结束");
    }
    bool hold(Job& j,bool keyboard,int code,bool down) {
        if(code<=0){lastInputError_=u8"按键无效，请重新选择键盘键或鼠标按钮";return false;}
        const auto key=std::make_pair(keyboard,code);
        if(down==bool(j.held.count(key)))return true;
        if(j.dry) {if(down)j.held.insert(key);else j.held.erase(key);return true;}
        if(down) {
            j.held.insert(key);const bool first=owners_[key]++==0;
            const bool ok=!first||host_.button(keyboard,code,true);
            if(!ok&&host_.inputError)lastInputError_=host_.inputError(keyboard,code);
            return ok;
        }
        bool ok=true;auto it=owners_.find(key);
        if(it!=owners_.end()&&--it->second<=0) {ok=host_.button(keyboard,code,false);owners_.erase(it);}
        j.held.erase(key);
        if(!ok&&host_.inputError)lastInputError_=host_.inputError(keyboard,code);
        return ok;
    }
    bool releasePulse(Job& j){bool ok=true;for(auto it=j.pulseKeys.rbegin();it!=j.pulseKeys.rend();++it)ok=hold(j,it->first,it->second,false)&&ok;j.pulseKeys.clear();return ok;}
    void releaseHeld(Job& j) {
        const auto held=j.held;
        for(const auto& k:held)if(!hold(j,k.first,k.second,false))
            note(j.program.name+u8"：设备未确认释放输入"+
                 (lastInputError_.empty()?std::string{}:u8"："+lastInputError_));
    }
    void cleanup() {
        for(auto& j:jobs_)if(j->done)releaseHeld(*j);
        jobs_.erase(std::remove_if(jobs_.begin(),jobs_.end(),[](const auto& j){return j->done;}),jobs_.end());
        const bool outputNeeded=std::any_of(jobs_.begin(),jobs_.end(),
            [](const auto& j){return !j->done&&!j->dry&&j->ownsOutput;});
        if(!outputNeeded&&acquired_) {host_.release(moved_);acquired_=false;moved_=false;}
    }
    bool start(const Program& p,const RuleSnapshot& s,bool dry,uint64_t parent,bool branch=false) {
        if(jobs_.size()>=32){note(u8"并发流程已达 32 个上限");return false;}
        auto flow=compileFlow(p);if(!flow.error.empty()||p.actions.empty()){note(p.name+u8"："+(flow.error.empty()?u8"没有动作":flow.error));return false;}
        if(!branch&&!conditionsMatch(p,s)){note(p.name+u8"：启动条件未满足");return false;}
        if(!parent) {
            std::vector<std::shared_ptr<Job>> victims;
            for(auto& j:jobs_)if(!j->done) {
                if(j->program.id==p.id) {
                    if(p.mode==Mode::Toggle)end(*j,u8"再次触发停止");
                    else if(p.mode==Mode::Sequence)j->paused=false;
                    return false;
                }
                const bool conflict=!number(p,"parallel")||!number(j->program,"parallel")||
                    (!option(p,"mutex").empty()&&option(p,"mutex")==option(j->program,"mutex"));
                if(conflict) {
                    if(number(p,"preempt")&&number(p,"priority")>number(j->program,"priority"))victims.push_back(j);
                    else {note(p.name+u8"：执行锁被占用");return false;}
                }
            }
            for(auto& victim:victims)end(*victim,u8"被高优先级宏抢占");
        } else if(!branch) {
            for(const auto& other:jobs_)if(!other->done&&other->program.id==p.id){note(u8"被调用规则已在执行，不能重入或递归");return false;}
            uint64_t ancestor=parent;int depth=0;
            while(ancestor) {
                bool found=false;for(const auto& j:jobs_)if(j->uid==ancestor) {
                    if(j->program.id==p.id||++depth>8){note(u8"宏调用循环或深度超过 8");return false;}
                    ancestor=j->parent;found=true;break;
                }if(!found)break;
            }
        }
        cleanup();
        auto j=std::make_shared<Job>();j->program=p;j->flow=std::move(flow);j->initial=s;
        j->uid=nextUid_++;j->parent=parent;j->started=s.now;j->dry=dry;j->manual=parent!=0||dry;
        const int minimum=std::max(0,number(p,"delay_min_ms")),maximum=std::max(minimum,number(p,"delay_max_ms"));
        const auto random=s.values.find("random.percent");const double r=random==s.values.end()?0:random->second.number/100;
        j->deadline=s.now+minimum+int((maximum-minimum)*std::clamp(r,0.,1.));
        jobs_.push_back(j);if(!parent)gates_[p.id].started(p,s);
        ++stats_[p.id].starts;
        note(p.name+(dry?u8"：开始模拟（不发送输入）":u8"：开始执行"));return true;
    }
    bool execute(Job& j,RuleSnapshot s) {
        if(!j.dry&&host_.nowMs)s.now=std::max(s.now,host_.nowMs());
        const auto index=int(j.pc);const auto& a=j.program.actions[j.pc];size_t next=j.pc+1;
        ++stats_[j.program.id].steps;
        j.message=actionLabels()[int(a.type)];
        lastInputError_.clear();
        const auto cond=[&](int n){return truth(j.program,s,n);};
        if(!j.dry&&exclusiveOutput(a.type)) {
            if(!acquired_) {
                if(!host_.acquire || !host_.acquire()) {
                    end(j,u8"自动输出尚未释放，请稍后重试");return false;
                }
                acquired_=true;
            }
            j.ownsOutput=true;
        }
        bool ok=true;std::string error;
        switch(a.type) {
        case ActionType::Delay: {auto r=s.values.find("random.percent");double f=r==s.values.end()?0:r->second.number/100;
            j.deadline=s.now+std::clamp(a.a,0,60000)+int(std::max(0,a.b-a.a)*f);break;}
        case ActionType::KeyDown:case ActionType::KeyUp:{auto keys=split(a.key,'+');if(keys.empty()){ok=false;break;}
            if(a.type==ActionType::KeyUp)std::reverse(keys.begin(),keys.end());
            for(const auto& key:keys)if(!hold(j,true,hidKey(key),a.type==ActionType::KeyDown)){ok=false;break;}break;}
        case ActionType::MouseDown:case ActionType::MouseUp:ok=hold(j,false,a.a,a.type==ActionType::MouseDown);break;
        case ActionType::KeyPress:case ActionType::MouseClick: {
            const bool keyboard=a.type==ActionType::KeyPress;
            std::vector<std::pair<bool,int>> pulse;
            if(keyboard)for(const auto& key:split(a.key,'+'))pulse.push_back({true,hidKey(key)});else pulse.push_back({false,a.a});
            if(pulse.empty()){ok=false;break;}
            for(const auto& key:pulse)if(key.second<=0||j.held.count(key)||(!j.dry&&owners_.count(key))){ok=false;error=u8"点按键无效或已被宏按住，请先松开或错开执行时间";break;}
            if(!ok)break;
            for(const auto& key:pulse)if(!hold(j,key.first,key.second,true)){ok=false;break;}
            j.pulse=ok;j.pulseKeys=std::move(pulse);
            const auto sentAt=!j.dry&&host_.nowMs?std::max(s.now,host_.nowMs()):s.now;
            j.deadline=sentAt+std::clamp(a.b,1,60000);break;
        }
        case ActionType::Loop:case ActionType::While:case ActionType::Retry:case ActionType::ForTargets: {
            auto& count=j.counters[index];
            if(a.type==ActionType::ForTargets && count==0) {
                j.previousTarget[index]=j.target;auto& ids=j.targetIds[index];ids.clear();for(const auto& t:s.targets)if(a.c<0||t.classId==a.c)ids.push_back(t.id);
            }
            const bool leave=a.type==ActionType::Loop?count>=std::clamp(a.a,0,1000000):
                a.type==ActionType::While?!cond(a.a):a.type==ActionType::Retry?cond(a.a):count>=int(j.targetIds[index].size());
            if(leave) {next=j.flow.end[index]+1;j.counters.erase(index);if(a.type==ActionType::ForTargets){j.target=j.previousTarget[index];j.targetIds.erase(index);j.previousTarget.erase(index);}}
            else {
                if(a.type==ActionType::Retry&&count>=std::clamp(a.b,1,10000)){ok=false;error=u8"已达到最大重试次数";break;}
                if(a.type==ActionType::ForTargets)j.target=j.targetIds[index][count];
                j.loopIndex=++count;variables_["loop.index"]=count;
            }break;
        }
        case ActionType::EndLoop: {
            const int begin=j.flow.begin[index];next=begin;
            if(j.program.actions[begin].type==ActionType::Retry)j.deadline=s.now+std::max(1,j.program.actions[begin].c);
            break;
        }
        case ActionType::Break:{const int begin=j.flow.begin[index];next=j.flow.end[begin]+1;j.counters.erase(begin);if(j.previousTarget.count(begin)){j.target=j.previousTarget[begin];j.previousTarget.erase(begin);j.targetIds.erase(begin);}break;}
        case ActionType::Continue:next=j.flow.end[j.flow.begin[index]];break;
        case ActionType::If:if(!cond(a.a))next=j.flow.otherwise[index]>=0?j.flow.otherwise[index]+1:j.flow.end[index]+1;break;
        case ActionType::Else:next=j.flow.end[index]+1;break;
        case ActionType::EndIf:case ActionType::EndSwitch:break;
        case ActionType::WaitCondition:
            if(cond(a.a))j.waitStarted=0;
            else {if(!j.waitStarted)j.waitStarted=s.now;
                if(s.now-j.waitStarted>=std::max(1,a.b)){ok=false;error=u8"等待条件超时";}
                else {j.deadline=s.now+5;return false;}}break;
        case ActionType::Stop:end(j,u8"停止动作");return false;
        case ActionType::SetVariable:variables_[a.text]=a.value;break;
        case ActionType::AddVariable:{auto& v=variables_[a.text];
            if(a.c==1)v-=a.value;else if(a.c==2)v*=a.value;else if(a.c==3){if(a.value==0){ok=false;error=u8"变量除数不能为 0";}else v/=a.value;}
            else if(a.c==4)v=std::min(v,a.value);else if(a.c==5)v=std::max(v,a.value);else v+=a.value;
            if(!std::isfinite(v)){ok=false;error=u8"变量运算结果溢出";v=0;}break;}
        case ActionType::Log:note(j.program.name+u8"："+a.text);break;
        case ActionType::Jump:if(cond(a.b)){if(a.a<1||a.a>int(j.program.actions.size())){ok=false;error=u8"跳转步骤不存在";}else next=a.a-1;}break;
        case ActionType::Switch: {
            next=j.flow.end[index]+1;const double value=variables_[a.text];
            for(int k=index+1;k<j.flow.end[index];++k)if(j.flow.begin[k]==index && j.program.actions[k].type==ActionType::Case){
                if(j.program.actions[k].text=="default")next=k+1;
                else if(j.program.actions[k].value==value){next=k+1;break;}}break;
        }
        case ActionType::Case:next=j.flow.end[index]+1;break;
        case ActionType::Parallel: {
            Program child=j.program;child.options["delay_min_ms"]="0";child.options["delay_max_ms"]="0";
            child.actions.assign(j.program.actions.begin()+index+1,j.program.actions.begin()+j.flow.end[index]);
            for(auto& action:child.actions)if(action.type==ActionType::Jump)action.a-=index+1;
            child.mode=Mode::Once;ok=start(child,s,j.dry,j.uid,true);next=j.flow.end[index]+1;break;
        }
        case ActionType::EndParallel:break;
        case ActionType::Call: {
            auto it=std::find_if(programs_.begin(),programs_.end(),[&](const Program& p){return p.id==a.text||p.name==a.text;});
            ok=it!=programs_.end()&&start(*it,s,j.dry,j.uid);
            if(ok && !a.a)j.waitChild=jobs_.back()->uid;
            else if(!ok)error=u8"被调用的宏不存在、条件不满足或存在递归";break;
        }
        case ActionType::EnableRule:case ActionType::DisableRule:case ActionType::PauseRule:case ActionType::ResumeRule: {
            bool found=false;
            for(auto& p:programs_)if(p.id==a.text||p.name==a.text) {
                found=true;
                if(a.type==ActionType::EnableRule||a.type==ActionType::ResumeRule)p.enabled=true;
                if(a.type==ActionType::DisableRule||a.type==ActionType::PauseRule)disabled_.insert(p.id);else disabled_.erase(p.id);
                for(auto& other:jobs_)if(other->program.id==p.id) {
                    if(a.type==ActionType::DisableRule)end(*other,u8"规则被禁用");
                    else {other->paused=a.type==ActionType::PauseRule;if(other->paused){releaseHeld(*other);other->pulse=false;other->pulseKeys.clear();}}
                }
            }ok=found;if(!ok)error=u8"规则名称 / ID 不存在";break;
        }
        case ActionType::SmoothMove:case ActionType::CurveMove: {
            if(!j.moving){j.motionStarted=s.now;j.sentX=j.sentY=0;j.moving=true;}
            const double t=std::clamp(double(s.now-j.motionStarted)/std::max(1,a.c),0.,1.);
            const double f=t*t*(3-2*t),bend=a.type==ActionType::CurveMove?std::sin(t*3.141592653589793)*a.d:0;
            const int x=int(std::lround(a.a*f)),y=int(std::lround(a.b*f+bend));
            Action step;step.type=ActionType::MouseMove;step.a=x-j.sentX;step.b=y-j.sentY;
            if(!j.dry && (step.a||step.b))ok=host_.action(step,s,error),moved_=true;
            j.sentX=x;j.sentY=y;if(!ok)break;
            if(t<1){j.deadline=s.now+5;return false;}j.moving=false;break;
        }
        default:
            if(!j.dry){if(host_.asyncAction)j.async=host_.asyncAction(a,j.alive);if(!j.async.valid())ok=host_.action(a,s,error);}
            if(a.type==ActionType::MouseMove||a.type==ActionType::AbsoluteMove||a.type==ActionType::MoveToTarget||a.type==ActionType::MoveToPrediction)moved_=true;
            break;
        }
        if(!ok) {
            if(error.empty())error=lastInputError_.empty()?u8"动作失败，已停止并释放本宏输入":lastInputError_;
            end(j,u8"步骤 "+std::to_string(index+1)+u8"（"+j.message+u8"）："+error);return false;
        }
        note((j.dry?u8"[模拟] ":"")+j.program.name+u8" · 步骤 "+std::to_string(index+1)+u8"："+j.message);
        j.pc=next;
        if(!j.manual&&j.program.mode==Mode::Sequence&&j.pc<j.program.actions.size())j.paused=true;
        return true;
    }
};
}
