#include "macro/rule_executor.h"
#include "macro/input_diagnostics.h"
#include "mouse/ferrum_protocol.h"
#include <cstdio>
using namespace macros;
int main(){int failures=0;auto check=[&](bool b,const char* n){if(!b){++failures;std::printf("FAIL %s\n",n);}};
    Program p;p.id="test";p.name="test";p.enabled=true;p.trigger="X1MouseButton";
    p.options["event"]="target_found";Condition key;key.metric="key";key.text="X1MouseButton";p.conditions={key};
    Action wait;wait.type=ActionType::Delay;wait.a=wait.b=50;
    Action press;press.type=ActionType::KeyPress;press.key="U";press.b=50;p.actions={wait,press};
    RuleSnapshot s;s.now=100;s.width=s.height=320;
    RuleGate gate;check(!gate.poll(p,s),"first observation seeds edges");
    s.targets.push_back({1,0,160,160,20,40,.9});s.targetId=1;s.now+=10;
    check(!gate.poll(p,s),"target gain without held side key does not start");
    s.keys.insert("X1MouseButton");s.now+=10;check(!gate.poll(p,s),"pressing after gain does not replay old target edge");
    s.targets.clear();s.targetId=-1;s.now+=10;gate.poll(p,s);
    s.targets.push_back({2,0,160,160,20,40,.9});s.targetId=2;s.now+=10;check(gate.poll(p,s),"new target with held side key starts");
    std::vector<std::pair<int,bool>> buttons;int acquired=0,released=0;
    RuleHost host;host.acquire=[&]{++acquired;return true;};host.release=[&](bool){++released;};
    host.button=[&](bool,int k,bool down){buttons.push_back({k,down});return true;};
    host.action=[](const Action&,const RuleSnapshot&,std::string&){return true;};
    RuleExecutor engine(host);engine.configure({p});check(engine.run(p.id,s),"manual start");
    engine.tick(s,true);check(buttons.empty(),"wait does not press early");
    check(acquired==0,"initial delay does not prematurely suspend automatic aim");
    s.now+=49;engine.tick(s,true);check(buttons.empty(),"49ms remains waiting");
    ++s.now;engine.tick(s,true);check(buttons.size()==1&&buttons[0]==std::make_pair(hidKey("U"),true),"U pressed at 50ms");
    s.now+=50;engine.tick(s,true);check(buttons.size()==2&&!buttons.back().second&&!engine.active(),"U released after 50ms and completes");
    check(acquired==1&&released==1,"output ownership balanced");
    p.conditions.clear();p.actions.clear();Action loop;loop.type=ActionType::Loop;loop.a=2;
    Action end;end.type=ActionType::EndLoop;p.actions={loop,loop,press,end,end};engine.configure({p});buttons.clear();engine.run(p.id,s);
    for(int i=0;i<30;++i){s.now+=50;engine.tick(s,true);}check(buttons.size()==8&&!engine.active(),"nested two by two loop executes four key presses");
    p.actions={loop,press};check(!compileFlow(p).error.empty(),"unclosed loop rejected");
    Condition notGroup;notGroup.metric="none";Condition unknown;unknown.parent=0;unknown.metric="ocr.text";unknown.text="missing";unknown.comparison="contains";
    p.conditions={notGroup,unknown};check(!evaluate(p,s).known&&!conditionsMatch(p,s),"NOT unknown OCR never triggers");
    p.conditions.clear();p.actions={press};engine.configure({p});buttons.clear();engine.run(p.id,s,true);
    engine.tick(s,true);s.now+=50;engine.tick(s,true);check(buttons.empty(),"dry run emits no input");

    // Parallel rules retain independent key ownership.
    Action down;down.type=ActionType::KeyDown;down.key="U";
    Action up=down;up.type=ActionType::KeyUp;
    p.options.clear();p.options["parallel"]="1";p.actions={down,wait,up};
    Program other=p;other.id="other";other.actions={down,wait,wait,up};
    engine.configure({p,other});buttons.clear();check(engine.run(p.id,s)&&engine.run(other.id,s),"parallel rules start");engine.tick(s,true);
    check(buttons.size()==1&&buttons[0].second,"shared key pressed only once");
    s.now+=50;engine.tick(s,true);check(buttons.size()==1,"first owner cannot release second owner");
    s.now+=50;engine.tick(s,true);check(buttons.size()==2&&!buttons.back().second,"last owner releases key");
    // A cancelled child and its parent cannot leak input.
    Action parallel;parallel.type=ActionType::Parallel;Action join;join.type=ActionType::EndParallel;
    p.actions={parallel,down,wait,join,wait};engine.configure({p});buttons.clear();engine.run(p.id,s);engine.tick(s,true);++s.now;engine.tick(s,true);engine.stop();
    check(buttons.size()==2&&!buttons.back().second&&!engine.active(),"cancelling parallel parent releases child key");
    // Action-only predicates allow false branches without blocking rule startup.
    Condition variable;variable.metric="variable";variable.text="counter";variable.value=3;variable.parent=-2;
    p.conditions={variable};Action set;set.type=ActionType::SetVariable;set.text="counter";set.value=0;
    Action increment=set;increment.type=ActionType::AddVariable;increment.value=1;
    Action branch;branch.type=ActionType::If;branch.a=1;Action otherwise;otherwise.type=ActionType::Else;Action endif;endif.type=ActionType::EndIf;
    Action fail;fail.type=ActionType::SetVariable;fail.text="wrong";fail.value=1;
    loop.a=3;p.actions={set,loop,increment,end,branch,press,otherwise,fail,endif};engine.configure({p});buttons.clear();check(engine.run(p.id,s),"action-only predicate doesn't block startup");
    for(int i=0;i<8;++i){s.now+=50;engine.tick(s,true);}check(engine.variables().at("counter")==3&&buttons.size()==2&&!engine.variables().count("wrong"),"loop variable and if else observe latest value");
    // Nested target traversal keeps outer iteration state.
    s.targets={{1,0,30,30,20,20,.8},{2,0,100,100,20,20,.9}};s.targetId=1;
    Action each;each.type=ActionType::ForTargets;each.c=-1;p.conditions.clear();p.actions={set,each,each,increment,end,end};engine.configure({p});engine.run(p.id,s);
    for(int i=0;i<8;++i){++s.now;engine.tick(s,true);}check(!engine.active()&&engine.variables().at("counter")==4,"nested target iteration runs cartesian count");
    // EnableRule can activate a rule originally disabled in configuration.
    other.enabled=false;other.options.clear();other.options["event"]="conditions";other.options["level"]="1";other.actions={press};other.conditions.clear();
    Action enable;enable.type=ActionType::EnableRule;enable.text=other.id;p.actions={enable};engine.configure({p,other});buttons.clear();engine.run(p.id,s);engine.tick(s,true);
    ++s.now;engine.tick(s,false);++s.now;engine.tick(s,false);check(!buttons.empty(),"enabling a disabled rule works");engine.stop();
    // Per-rule template events use their own path, not another rule's global hit.
    p.options.clear();p.options["event"]="image_found";p.options["pattern"]="correct.png";p.actions={press};
    Condition image;image.metric="image.hit";image.text="correct.png";image.upper=.9;
    engine.configure({p});buttons.clear();s.values[sensorKey(image)]=Value::numeric(0);engine.tick(s);
    s.values["image.hit"]=Value::numeric(1);++s.now;engine.tick(s);check(buttons.empty(),"unrelated template cannot trigger rule");
    s.values[sensorKey(image)]=Value::numeric(1);++s.now;engine.tick(s);check(buttons.size()==1,"matching template triggers");engine.stop();
    // Unknown data, target loss, cooldown, held locks, recursion and invalid references.
    p.options.clear();p.options["event"]="key_down";p.options["key_lock"]="1";p.trigger="LeftControl+U";RuleGate chord;
    s.keys.clear();chord.poll(p,s);s.keys={"LeftControl","U"};++s.now;check(chord.poll(p,s),"chord edge");chord.started(p,s);
    s.keys.erase("U");++s.now;chord.poll(p,s);s.keys.insert("U");++s.now;check(!chord.poll(p,s),"chord lock until all keys released");
    s.keys.clear();++s.now;chord.poll(p,s);s.keys={"LeftControl","U"};++s.now;check(chord.poll(p,s),"all up resets chord lock");
    branch.a=-1;p.actions={branch,endif};check(!compileFlow(p).error.empty(),"deleted condition reference rejected");
    Action call;call.type=ActionType::Call;call.text=p.id;p.actions={call};engine.configure({p});engine.run(p.id,s);engine.tick(s,true);check(!engine.active(),"recursive macro rejected without hang");
    p.options.clear();p.options["timeout_ms"]="20";p.actions={down,wait};engine.configure({p});buttons.clear();engine.run(p.id,s);engine.tick(s,true);s.now+=21;engine.tick(s,true);
    check(!engine.active()&&buttons.size()==2&&!buttons.back().second,"timeout releases held key");

    p.options.clear();p.conditions.clear();p.mode=Mode::Once;p.actions={press};p.actions[0].key="LeftControl+U";engine.configure({p});buttons.clear();engine.run(p.id,s);engine.tick(s,true);s.now+=50;engine.tick(s,true);
    check(buttons.size()==4&&buttons[0]==std::make_pair(hidKey("LeftControl"),true)&&buttons[3]==std::make_pair(hidKey("LeftControl"),false),"chord press releases in reverse order");
    p.mode=Mode::Sequence;p.trigger="U";p.actions={press,press};engine.configure({p});buttons.clear();s.keys.clear();engine.tick(s);s.keys.insert("U");++s.now;engine.tick(s);s.keys.clear();s.now+=50;engine.tick(s);
    check(engine.active()&&buttons.size()==2,"sequence waits for next press after first step");s.keys.insert("U");++s.now;engine.tick(s);s.now+=50;engine.tick(s);check(!engine.active()&&buttons.size()==4,"sequence finishes last step without an extra press");
    p.mode=Mode::Once;p.options["event"]="image_found";p.options["pattern"]="correct.png";p.actions={press};engine.configure({p});buttons.clear();s.values.erase(sensorKey(image));engine.tick(s);
    s.values[sensorKey(image)]=Value::numeric(1);++s.now;engine.tick(s);check(buttons.size()==1,"first completed positive vision result triggers found event");engine.stop();
    p.options.clear();p.options["event"]="conditions";p.options["level"]="1";p.options["cancel_conditions"]="0";
    Condition cooldown;cooldown.metric="cooldown.ready";p.conditions={cooldown};engine.configure({p});buttons.clear();engine.tick(s);++s.now;engine.tick(s);check(buttons.size()==1,"cooldown condition retains context at start");engine.stop();

    std::promise<std::string> completed;auto future=completed.get_future().share();std::shared_ptr<std::atomic<bool>> alive;
    host.asyncAction=[&](const Action& action,std::shared_ptr<std::atomic<bool>> flag){if(action.type!=ActionType::ExportConfig)return std::shared_future<std::string>{};alive=flag;return future;};
    RuleExecutor asyncEngine(host);p.options.clear();p.conditions.clear();Action exportAction;exportAction.type=ActionType::ExportConfig;p.actions={exportAction,press};asyncEngine.configure({p});buttons.clear();asyncEngine.run(p.id,s);asyncEngine.tick(s,true);++s.now;asyncEngine.tick(s,true);
    check(buttons.empty()&&asyncEngine.active(),"flow waits for asynchronous system action");completed.set_value({});++s.now;asyncEngine.tick(s,true);check(buttons.size()==1,"next action starts only after async completion");asyncEngine.stop();check(alive&&!alive->load(),"stop cancels queued system action token");
    // A looping prediction/aim-parameter macro must leave the consumer running.
    {
        bool outputOwned=false;int controls=0,presses=0;
        RuleHost h;h.acquire=[&]{outputOwned=true;return true;};h.release=[&](bool){outputOwned=false;};
        h.action=[&](const Action&,const RuleSnapshot&,std::string&){++controls;return true;};
        h.button=[&](bool,int,bool){++presses;return true;};
        RuleExecutor e(h);Program control;control.id="control";control.enabled=true;
        control.trigger="F1";control.mode=Mode::Hold;control.options["parallel"]="1";
        Action prediction;prediction.type=ActionType::Prediction;prediction.value=20;
        control.actions={prediction};Program physical=control;physical.id="physical";
        physical.trigger="F2";physical.mode=Mode::Once;physical.actions={press};
        e.configure({control,physical});RuleSnapshot input;input.now=1000;
        e.tick(input);input.keys.insert("F1");++input.now;e.tick(input);
        check(e.active()&&controls==1&&!outputOwned,"held control macro changes parameters without stopping aim");
        input.now+=30;e.tick(input);input.now+=30;e.tick(input);
        check(controls>=2&&!outputOwned,"looping control macro remains effective while aim continues");
        input.keys.insert("F2");++input.now;e.tick(input);
        check(outputOwned&&presses==1,"physical macro still acquires exclusive input ownership");
        input.now+=60;e.tick(input);
        check(e.active()&&!outputOwned&&presses==2,
              "finishing physical macro resumes aim even while parallel control macro remains active");
        input.keys.clear();++input.now;e.tick(input);check(!e.active(),"held macro ends when its key is released");
    }
    // Ferrum COM writes can outlast the requested press. Never count queue /
    // transport time as the key's actual hold, including a down + wait + up flow.
    for(bool keyboard:{true,false}) {
        int64_t clock=5000;
        std::vector<std::pair<std::string,int64_t>> wire;
        RuleHost h;h.nowMs=[&]{return clock;};
        h.acquire=[&]{clock+=25;return true;};h.release=[](bool){};
        h.button=[&](bool key,int code,bool pressed) {
            clock+=80;
            const auto command=key?mouse_driver::ferrum_protocol::keyCommand({code},pressed):
                std::string("km.left(")+(pressed?"1)":"0)");
            wire.push_back({command,clock});return true;
        };
        RuleExecutor e(h);Program flow;flow.id="fe";flow.enabled=true;
        Action click;click.type=keyboard?ActionType::KeyPress:ActionType::MouseClick;
        click.key="U";click.a=1;click.b=50;flow.actions={click};
        e.configure({flow});RuleSnapshot input;input.now=clock;e.run(flow.id,input);e.tick(input,true);
        check(wire.size()==1,"slow FE send emits a down command before scheduling its release");
        const auto pressedAt=clock;input.now=clock=pressedAt+49;e.tick(input,true);
        check(wire.size()==1,"FE press stays down for the full requested hold after send completion");
        input.now=clock=pressedAt+50;e.tick(input,true);
        check(wire.size()==2&&!e.active()&&wire[1].second-wire[0].second>=50,
              "keyboard and mouse FE presses release only after the requested duration");
        check(wire[0].first==(keyboard?"km.down(24)":"km.left(1)")&&
              wire[1].first==(keyboard?"km.up(24)":"km.left(0)"),
              "FE emits the official press and release commands with HID/button units");

        wire.clear();click.type=keyboard?ActionType::KeyDown:ActionType::MouseDown;
        Action up=click;up.type=keyboard?ActionType::KeyUp:ActionType::MouseUp;
        flow.actions={click,wait,up};e.configure({flow});input.now=clock;e.run(flow.id,input);e.tick(input,true);
        const auto downAt=clock;input.now=clock=downAt+49;e.tick(input,true);
        check(wire.size()==1,"wait after a slow FE down starts after its transport completes");
        input.now=clock=downAt+50;e.tick(input,true);
        check(wire.size()==2&&!e.active(),"FE down-wait-up sequence releases without a leaked hold");
    }
    {
        RuleHost h;h.acquire=[] {return true;};h.release=[](bool){};
        h.button=[](bool,int,bool){return false;};
        h.inputError=[](bool,int){return std::string("FE command failed: check serial connection");};
        RuleExecutor e(h);Program flow;flow.id="failure";flow.enabled=true;flow.actions={press};
        e.configure({flow});e.run(flow.id,s);e.tick(s,true);
        check(!e.active()&&e.log().back().find("FE command failed")!=std::string::npos,
              "input transport failure reaches the macro's failing-step diagnostics");
    }
    // 滚轮不是一个可“按住”的键：固定 id、按格输出、没有 HID 码。
    check(wheelKeyId("WheelUp") && wheelKeyId("WheelDown"), "wheel keys are known ids");
    check(!wheelKeyId("U") && !wheelKeyId(""), "ordinary keys are not wheel keys");
    check(wheelNotches("WheelUp") == 1 && wheelNotches("WheelDown") == -1 && wheelNotches("U") == 0,
          "wheel output is one notch up or down");
    check(hidKey("WheelUp") == 0 && wheelKeys().size() == 2,
          "wheel keys carry no HID code and are listed for the UI");
    std::printf("macro rules: %d failures\n",failures);return failures?1:0;}
