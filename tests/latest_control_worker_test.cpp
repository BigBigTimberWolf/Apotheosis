#include "runtime/latest_control_worker.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
using namespace std::chrono_literals;
int main() {
    std::mutex m;std::condition_variable cv;
    bool entered=false,release=false;int calls=0,seen=0;
    std::atomic<int> latest{1};
    const auto producer=std::this_thread::get_id();bool separate=false;
    {
        runtime::LatestControlWorker worker([&] {
            std::unique_lock lock(m);
            separate = std::this_thread::get_id()!=producer;
            ++calls;
            if(calls==1) {
                entered=true;cv.notify_all();
                cv.wait(lock,[&]{return release;});
            }
            seen=latest.load();cv.notify_all();
        });
        worker.notify();
        {
            std::unique_lock lock(m);
            if(!cv.wait_for(lock,2s,[&]{return entered;})) {
                release=true;cv.notify_all();return 1;
            }
        }
        // Producer remains able to publish while control is blocked. A burst
        // coalesces to one pending update, never replays all obsolete batches.
        for(int i=2;i<=100;++i){latest=i;worker.notify();}
        {
            std::unique_lock lock(m);release=true;cv.notify_all();
            if(!cv.wait_for(lock,2s,[&]{return calls==2;}))return 2;
        }
    } // idle shutdown must wake and join
    if(calls!=2 || seen!=100 || !separate)return 3;
    std::puts("independent latest-only control worker passed");
}
