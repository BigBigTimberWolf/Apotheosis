#include "runtime/background_motion_estimator.h"
#include <chrono>
#include <cstdio>
using namespace control;
int main(int argc, char**) {
    int failures=0;
    auto check=[&](bool ok,const char* text) { if(!ok) { ++failures; std::printf("FAIL: %s\n",text); } };
    if (argc>1) cv::setNumThreads(1);
    cv::Mat base(320,320,CV_8UC1); cv::RNG rng(867);
    rng.fill(base,cv::RNG::UNIFORM,0,255); cv::GaussianBlur(base,base,{5,5},1.1);
    auto frame=[&](double x,double y) {
        cv::Mat result;
        cv::warpAffine(base,result,cv::Matx23d(1,0,x,0,1,y),base.size(),cv::INTER_LINEAR,cv::BORDER_REFLECT101);
        return result;
    };
    runtime::BackgroundMotionEstimator estimator;
    BackgroundMotion bg;
    double totalMs=0,maxMs=0;
    int accepted=0;
    for(int n=0;n<80;++n) {
        auto image=frame(-2*n,n);
        const cv::Rect target(125+(n%8),120,55,75);
        image(target).setTo(n%2?220:30);
        cv::Mat gray; runtime::BackgroundMotionEstimator::thumbnail(image,gray);
        const auto start=std::chrono::steady_clock::now();
        bg=estimator.update(gray,image.size(),{target},1000000+10000*n);
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        totalMs+=ms; maxMs=std::max(maxMs,ms);
        if(n>0) {
            check(bg.valid,"translated background remains reliable despite foreground movement");
            accepted+=bg.valid;
            check(std::abs(bg.cumulative.x+2*n)<2 && std::abs(bg.cumulative.y-n)<2,
                  "background displacement has correct sign and original-image scale");
        }
    }
    const auto duplicate=estimator.update(frame(0,0),{320,320},{},bg.timeUs);
    check(duplicate.chain==bg.chain && duplicate.cumulative.x==bg.cumulative.x,"duplicate frame is not counted twice");
    cv::Mat flat(160,160,CV_8UC1,cv::Scalar(127));
    const auto lost=estimator.update(flat,{320,320},{},2000000);
    check(!lost.valid && lost.chain!=bg.chain,"gap breaks the background chain");
    check(!estimator.update(flat,{320,320},{},2010000).valid,"flat background stays unknown");
    estimator.reset();
    cv::Mat gray; runtime::BackgroundMotionEstimator::thumbnail(base,gray);
    estimator.update(gray,{320,320},{},3000000);
    check(!estimator.update(gray,{320,320},{{0,0,320,320}},3010000).valid,"full foreground coverage cannot become camera evidence");
    estimator.reset();
    estimator.update(gray,{320,320},{},4000000);
    cv::Mat unrelated(160,160,CV_8UC1); rng.fill(unrelated,cv::RNG::UNIFORM,0,255);
    check(!estimator.update(unrelated,{320,320},{},4010000).valid,"scene cut does not assert camera motion");

    TargetDirectionObserver direction;
    // Inference sizes are not always integer multiples of the thumbnail.
    for(int side : {256,416,640}) {
        cv::Mat texture(side,side,CV_8UC1); rng.fill(texture,cv::RNG::UNIFORM,0,255);
        cv::GaussianBlur(texture,texture,{5,5},1.1);
        runtime::BackgroundMotionEstimator scaled;
        BackgroundMotion previous;
        for(int n=0;n<12;++n) {
            cv::Mat shifted,small;
            cv::warpAffine(texture,shifted,cv::Matx23d(1,0,-2*n,0,1,n),texture.size(),
                           cv::INTER_LINEAR,cv::BORDER_REFLECT101);
            runtime::BackgroundMotionEstimator::thumbnail(shifted,small);
            const auto current=scaled.update(small,texture.size(),{},7000000+n*16000);
            if(n>0) {
                const auto d=current.cumulative-previous.cumulative;
                check(current.valid && std::abs(d.x+2)<.75 && std::abs(d.y-1)<.75,
                      "noninteger thumbnail scaling preserves per-frame background displacement");
            }
            previous=current;
        }
    }
    bg={}; bg.valid=true; bg.chain=8;
    auto step=[&](Vec2 target,Vec2 camera,int n) {
        bg.cumulative=camera; bg.variance=n*.01; bg.timeUs=5000000+n*10000;
        return direction.update(target,{40,70},bg,bg.timeUs);
    };
    for(int n=0;n<8;++n) {
        const auto reverse=step({100-2.0*n,100},{-3.0*n,0},n);
        check(reverse.x==0,"target still moving right despite leftward screen movement is not reversed");
    }
    check(step({82,100},{-24,0},8).x==0,"one opposite observation cannot clear lead");
    auto reversed=step({78,100},{-27,0},9);
    check(reversed.x==1 && reversed.y==0,"confirmed corrected target reversal clears only X");
    check(step({74,100},{-30,0},10).x==0,"continued same direction does not reset repeatedly");
    check(step({66,100},{-36,0},12).x==0,"cumulative background handles skipped control frames");
    bg.valid=false; bg.timeUs+=10000;
    check(direction.update({0,0},{40,70},bg,bg.timeUs).x==0,"unknown camera does not fabricate reversal");
    direction.reset();
    for(int n=0;n<40;++n) {
        const auto r=step({100+(n%2?.2:-.2),100},{0,0},n);
        check(r.x==0 && r.y==0,"alternating detection jitter is not a turn");
    }
    std::printf("background motion: %d failures; synthetic 160px LK %.3f ms mean, %.3f ms max; accepted %d/79\n",
                failures,totalMs/80,maxMs,accepted);
    return failures?1:0;
}
