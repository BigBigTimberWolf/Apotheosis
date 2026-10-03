#include "capture/auto_capture_policy.h"
#include <cstdio>
int main() {
    int failures=0;
    auto check=[&](bool okay,const char* message){if(!okay){std::printf("FAIL: %s\n",message);++failures;}};
    AutoCapture::Policy p; p.enabled=true;
    check(p.accepts(true,false,false,1,{.9f}),"normal high-confidence capture is preserved");
    check(!p.accepts(true,false,false,1,{.2f}),"normal thresholds still apply");
    check(p.accepts(false,true,false,0,{}),"normal force key still captures without inference");
    p.anyDetection=true;
    check(p.accepts(true,false,false,1,{}),"any-detection mode does not require confidence metadata");
    p.triggerOnly=true;
    check(!p.accepts(true,true,false,1,{.99f}),"trigger-only mode cannot be bypassed by confidence or force keys");
    check(p.accepts(false,false,true,1,{.2f}),"confirmed trigger is captured even after a newer detection arrived");
    check(p.accepts(false,false,true,0,{}),"continuous fire may capture an unlabelled frame");
    p.enabled=false;
    check(!p.accepts(true,true,true,1,{.9f}),"global disable wins over trigger and force");

    AutoCapture::TriggerSamples samples;
    const runtime::FrameContext shotFrame{7,1234567,320,320};
    samples.push({shotFrame,{{{1,2,3,4},5,.8}}});
    check(samples.pending(),"shot notification wakes the writer without a new detection");
    const auto first=samples.pop();
    check(first && first->context.sequence==7 && first->context.captured_ns==1234567 &&
          first->detections[0].classId==5 && first->detections[0].box.x==1,
          "shot frame and labels remain paired rather than borrowing the latest detection");
    check(!samples.pending()&&!samples.pop(),"a shot is consumed once");
    samples.push({{}, {}}); check(!samples.pending(),"invalid frame identity is not queued");
    for(int i=1;i<=20;++i)samples.push({{uint64_t(i),i,320,320},{}});
    check(samples.pop()->context.sequence==5,"bounded queue discards the oldest shot under backpressure");
    samples.clear();check(!samples.pending()&&!samples.pop(),"stop clears queued shot captures");
    return failures?1:0;
}
