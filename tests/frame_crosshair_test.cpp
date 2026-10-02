#include "runtime/frame_crosshair.h"
#include <cstdio>
#include <limits>
int main() {
    runtime::FrameContext oldFrame{10,1000000,400,200}, newFrame{11,2000000,400,200};
    runtime::FrameCrosshair old{oldFrame,100,50,2,true}, next{newFrame,200,80,2,true};
    // Completion order may differ: an old batch retains its own pivot while
    // a newer capture/result exists. Neither may be paired with the other.
    auto hit = old.forDetection(oldFrame,2,200);
    if (!hit || hit->x!=50 || hit->y!=50) return 1;
    if (old.forDetection(newFrame,2,200) || next.forDetection(oldFrame,2,200)) return 2;
    if (old.forDetection(oldFrame,3,200)) return 3;
    old.valid=false;
    if(old.forDetection(oldFrame,2,200))return 4;
    old.valid=true; old.x=std::numeric_limits<double>::quiet_NaN();
    if(old.forDetection(oldFrame,2,200))return 5;
    old.x=100; auto wrong=oldFrame;wrong.captured_ns++;
    if(old.forDetection(wrong,2,200))return 6;
    old.frame={0,0,400,200};
    if(!old.forDetection(old.frame,2,200))return 7;
    old.frame.width=0;
    if(old.forDetection(old.frame,2,200))return 8;
    using Hold=runtime::CrosshairFrameHold;
    for(int algorithm : {0,1}) {
        Hold hold;
        runtime::FrameContext f{100,1000000,400,200};
        runtime::FrameCrosshair p{f,100,60,2,true};
        auto r=hold.resolve(f,p,2,200,algorithm,1);
        if(r.source!=Hold::Source::CurrentFrame || r.x!=50 || r.y!=60)return 9;
        for(int miss=1;miss<=4;++miss) {
            ++f.sequence; f.captured_ns+=1000000;
            r=hold.resolve(f,{},2,200,algorithm,miss+1);
            const auto expected=miss<=3?Hold::Source::PreviousFrame:Hold::Source::Center;
            if(r.source!=expected || r.x!=(miss<=3?50:100))return 10;
            // Multiple control ticks observing one batch do not age the hold.
            for(int repeat=0;repeat<5;++repeat)
                if(hold.resolve(f,{},2,200,algorithm,miss+1).source!=expected)return 11;
        }
        ++f.sequence; f.captured_ns+=1000000; p={f,140,70,2,true};
        r=hold.resolve(f,p,2,200,algorithm,6);
        if(r.source!=Hold::Source::CurrentFrame || r.x!=70)return 12;
        ++f.sequence; f.captured_ns+=1000000;
        r=hold.resolve(f,{},2,200,algorithm,7);
        if(r.source!=Hold::Source::PreviousFrame || r.x!=70)return 13;
        // Changing hotkeys or algorithms must not borrow another context's point.
        if(hold.resolve(f,{},3,200,algorithm,7).source!=Hold::Source::Center)return 14;
        p={f,140,70,3,true}; ++f.sequence; p.frame=f;
        hold.resolve(f,p,3,200,algorithm,8);
        if(hold.resolve(f,{},3,200,1-algorithm,8).source!=Hold::Source::Center)return 15;
        hold.reset(); p={f,140,70,2,true}; hold.resolve(f,p,2,200,algorithm,8);
        f.sequence+=4;
        if(hold.resolve(f,{},2,200,algorithm,9).source!=Hold::Source::Center)return 16;
        // An unrelated newer preview pivot can never seed the retention cache.
        hold.reset(); p.frame.sequence=f.sequence+1;
        if(hold.resolve(f,p,2,200,algorithm,10).source!=Hold::Source::Center)return 17;
        // With no capture IDs, detection batch versions still identify frames.
        hold.reset(); f={0,0,400,200}; p={f,140,70,2,true};
        hold.resolve(f,p,2,200,algorithm,1);
        for(int miss=1;miss<=4;++miss) {
            r=hold.resolve(f,{},2,200,algorithm,miss+1);
            if(r.source!=(miss<=3?Hold::Source::PreviousFrame:Hold::Source::Center))return 18;
        }
    }
    std::puts("same-frame crosshair pairing passed");
}
