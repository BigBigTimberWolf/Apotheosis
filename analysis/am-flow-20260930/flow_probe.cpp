// Offline plant simulation using the UNMODIFIED production PID and move slot.
// This does not execute AM, a game, a device driver, or capture/inference code.
#include "control/recovered_pid.h"
#include "control/recovered_aim_controller.h"
#include "runtime/motion_feedback_window.h"
#include "mouse/latest_move_slot.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <utility>
#include <vector>
using namespace control;
int main() {
  std::puts("kind,profile,hz,gain_px_per_count,feedback_delay_ms,first_move_ms,first_within_2px_ms,settled_100ms_start_ms,final_error_px,reversals,generated_counts,sent_counts");
  for(int profile=0;profile<3;++profile) for(int hz:{60,120,240})
  for(double gain:{0.25,0.5,1.0}) for(int delay:{0,4,8}) {
    RecoveredPidConfig cfg;
    if(profile==1) {cfg.kiX=cfg.kiY=.01f;cfg.kdX=cfg.kdY=.01f;cfg.smoothMaxPixel=416;}
    if(profile==2) {cfg.kiX=cfg.kiY=0;cfg.kdX=cfg.kdY=0;cfg.deadzoneX=cfg.deadzoneY=0;cfg.smoothMaxPixel=416;}
    RecoveredPid pid;pid.setConfig(cfg);
    double position=0, first=-1, hit=-1, runStart=-1,settled=-1,total=0;
    int sign=0,reversals=0;
    std::deque<std::pair<double,double>> history;
    history.push_back({-1,0});
    for(int n=0;n<=hz*3;++n) {
      double t=double(n)/hz, sample=0;
      for(const auto& h:history) {if(h.first<=t-delay*.001+1e-10)sample=h.second;else break;}
      auto step=pid.update({80-sample,0},{},1.0/hz);
      int move=step.counts.x;
      if(move&&first<0)first=t*1000;
      if(move) {int s=move>0?1:-1;if(sign&&sign!=s)++reversals;sign=s;}
      position+=move*gain; total+=move;
      history.push_back({t,position});
      if(std::abs(80-position)<=2) {
        if(hit<0)hit=t*1000;
        if(runStart<0)runStart=t;
        if(settled<0&&t-runStart>=.1-1e-10)settled=runStart*1000;
      } else runStart=-1;
    }
    settled=runStart>=0 && 3.0-runStart>=.1-1e-10 ? runStart*1000 : -1;
    std::printf("production_pid,%s,%d,%.2f,%d,%.3f,%.3f,%.3f,%.3f,%d,%.0f,%.0f\n",
      profile==0?"defaults":profile==1?"zhou_pid_values":"P_only_control",hz,gain,delay,first,hit,settled,80-position,reversals,total,total);
  }
  // Full production tracker -> anchor -> following compensation -> PID chain.
  // Static target, configurable ideal screen response; 80 px initial error.
  // zhou.ini hotkey.0 coefficients; target class 0, center aimpoint.
  for(bool follow:{false,true}) for(int hz:{60,120,240}) for(double gain:{0.25,0.5,1.0}) for(int delay:{0,4,8}) {
    ControllerConfig c;c.frameWidth=c.frameHeight=416;c.fovWidth=c.fovHeight=416;
    c.dynamicFovEnabled=false;c.buckets.byClassId={Bucket::Aim};
    RecoveredPidConfig p;p.kiX=p.kiY=.01f;p.kdX=p.kdY=.01f;
    p.smoothMaxPixel=416;p.followX=p.followY=follow?10.0f:0.0f;p.feedforwardX=p.feedforwardY=.001f;
    RecoveredAimController controller;controller.setConfig(c,p);
    double position=0,first=-1,hit=-1,runStart=-1,settled=-1,total=0;
    int sign=0,reversals=0;runtime::MotionFeedbackWindow feedback;
    std::deque<std::pair<double,double>> history;history.push_back({-1,0});
    for(int n=0;n<=hz*3;++n) {
      double t=double(n)/hz,sample=0;
      // Mirror aim_loop.cpp's first-tick initialization/return, not its I/O.
      if(n==0) {history.push_back({0,0});continue;}
      for(auto h:history){if(h.first<=t-delay*.001+1e-10)sample=h.second;else break;}
      ControlInput in;in.cross={208,208};in.dtSec=1.0/hz;in.frameIndex=n;
      in.observationTimeUs=1000000+static_cast<int64_t>((t-delay*.001)*1e6);
      in.detectionFresh=true;in.crosshairFresh=true;in.autoFire=true;
      in.motionEventSum=feedback.sample(in.observationTimeUs);
      in.candidates={Candidate{{278-sample,198,20,20},0,.99}};
      auto out=controller.update(in);int move=out.counts.x;
      if(move&&first<0)first=t*1000;
      if(move){int s=move>0?1:-1;if(sign&&sign!=s)++reversals;sign=s;}
      position+=move*gain;total+=move;history.push_back({t,position});
      feedback.add({move,0,1000000+static_cast<int64_t>(t*1e6),0});
      if(std::abs(80-position)<=2){if(hit<0)hit=t*1000;if(runStart<0)runStart=t;
        if(settled<0&&t-runStart>=.1-1e-10)settled=runStart*1000;
      }else runStart=-1;
    }
    settled=runStart>=0 && 3.0-runStart>=.1-1e-10 ? runStart*1000 : -1;
    std::printf("production_controller,%s,%d,%.2f,%d,%.3f,%.3f,%.3f,%.3f,%d,%.0f,%.0f\n",
      follow?"zhou_primary_chain":"zhou_follow_off_control",hz,gain,delay,first,hit,settled,80-position,reversals,total,total);
  }
  // Slow consumer experiment: use the real latest-only production slot.
  for(int consumerPeriod:{1,2,4}) {
    mouse_async::LatestMoveSlot slot;mouse_async::PendingMove move;
    int generated=0,sent=0;
    for(int n=0;n<120;++n) {
      slot.replace(10,0,{});generated+=10;
      if(n%consumerPeriod==consumerPeriod-1&&slot.take(move))sent+=move.dx;
    }
    if(slot.take(move))sent+=move.dx;
    std::printf("production_slot,consumer_every_%d_frames,120,0,0,0,0,0,0,0,%d,%d\n",consumerPeriod,generated,sent);
    if(sent!=1200/consumerPeriod)return 2;
  }
}
