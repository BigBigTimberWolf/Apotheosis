#include "ndi/latest_slot.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
void check(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
int main() {
    try {
        ndi::LatestSlot<std::shared_ptr<int>> slot;
        auto first=std::make_shared<int>(1);
        std::weak_ptr<int> weak=first;
        slot.publish(first); first.reset();
        slot.publish(std::make_shared<int>(2));
        check(weak.expired(), "replaced frame must release storage");
        auto inFlight=slot.take();
        check(inFlight && **inFlight==2,"latest frame must win");
        for(int i=3;i<100;++i) slot.publish(std::make_shared<int>(i));
        check(**inFlight==2,"publishing must not mutate in-flight async frame");
        auto next=slot.take();
        check(next && **next==99,"slow consumer must receive newest frame");
        check(!slot.take(),"frames must be consumed once");
        std::promise<void> ready;
        auto waiter=std::async(std::launch::async,[&]{ ready.set_value(); return slot.wait(5000); });
        ready.get_future().wait();
        slot.close();
        check(waiter.wait_for(std::chrono::seconds(1))==std::future_status::ready,"shutdown must wake waiters");
        check(!waiter.get(),"closed empty slot must not report a frame");
        slot.publish(std::make_shared<int>(100));
        check(!slot.take(),"closed slot must reject publications");
        std::cout << "NDI latest-slot ownership and shutdown passed\n";
        return 0;
    }catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
