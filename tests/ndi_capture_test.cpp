#include "capture/ndi_capture.h"
#include "ndi/ndi_runtime.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <filesystem>
#include <memory>
void check(bool ok,const char* what) { if(!ok) throw std::runtime_error(what); }
class TestSender {
public:
    TestSender(std::filesystem::path exe, std::string name, bool pattern=true)
        : exe_(std::move(exe)), name_(std::move(name)), pattern_(pattern) { start(); }
    ~TestSender() { stop(); }
    void stop() {
        if (process_) {
            TerminateProcess(process_, 0); // simulate abrupt network/source failure
            WaitForSingleObject(process_, 3000); CloseHandle(process_); process_=nullptr;
        }
    }
    void start() {
        std::wstring cmd=L"\""+exe_.wstring()+L"\" --name "+std::wstring(name_.begin(),name_.end())+
            (pattern_ ? L" --test-pattern" : L"") + L" --size 320 --fps 60 --seconds 30";
        STARTUPINFOW startup{}; startup.cb=sizeof(startup);
        PROCESS_INFORMATION process{};
        check(CreateProcessW(exe_.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process),"start test sender");
        CloseHandle(process.hThread); process_=process.hProcess;
    }
    bool running() const { DWORD code=0; return GetExitCodeProcess(process_, &code) && code==STILL_ACTIVE; }
private:
    std::filesystem::path exe_; std::string name_; bool pattern_; HANDLE process_=nullptr;
};
// With --sender, CTest starts its own unique source and tests source recovery.
// Without it, launch a source named ApotheosisTest manually.
int main(int argc,char** argv) {
    try {
        std::string error;
        if (!ndi::Runtime::Load(error)) { std::cout << "SKIP: " << error << "\n"; return 77; }
        std::unique_ptr<TestSender> sender;
        const bool desktop=argc==3 && std::string(argv[1])=="--desktop-sender";
        const std::string name=argc==3
            ? "ApotheosisTest_"+std::to_string(GetCurrentProcessId()) : "ApotheosisTest";
        if(argc==3) sender=std::make_unique<TestSender>(std::filesystem::path(argv[2]),name,!desktop);
        std::string selected;
        for (int retry=0;retry<5 && selected.empty();++retry) {
            for(const auto& discovered:ndi_capture::Discover(1500,error)) {
                std::cout << "Discovered: " << discovered << "\n";
                if (discovered.ends_with(" ("+name+")")) selected=discovered;
            }
            if(sender) check(sender->running(),"test sender exited unexpectedly");
            if(!error.empty()) std::cout << "Discovery error: " << error << "\n";
        }
        if (selected.empty()) {
            check(!sender,"test sender must be discoverable");
            std::cout << "SKIP: launch ndi_sender --name ApotheosisTest --test-pattern --fps 60\n"; return 77;
        }
        auto capture=ndi_capture::Create(selected,64);
        check(bool(capture),"receiver creation");
        cv::Mat first;
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(first.empty() && std::chrono::steady_clock::now()<end) {
            capture->WaitFrame(100); first=capture->GetNextFrameCpu();
        }
        check(!first.empty(),"receive first frame");
        check(first.cols==64 && first.rows==64 && first.type()==CV_8UC3,"crop dimensions and BGR format");
        const auto pixel=first.at<cv::Vec3b>(4,4);
        if(!desktop) check(std::abs(int(pixel[1])-132)<12 && std::abs(int(pixel[2])-132)<12,"center ROI and channel order");
        const int64_t timestamp=capture->GetLastFrameCaptureNs();
        check(timestamp>0,"receive timestamp");
        if(!desktop) {
            // Allow producer to outrun consumer: reading must not drain old history.
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            auto latest=capture->GetNextFrameCpu();
            check(!latest.empty(),"newest frame after slow consumer");
            check(capture->GetLastFrameCaptureNs()>timestamp+150000000,"old backlog must be replaced");
            check(first.at<cv::Vec3b>(4,4)==pixel,"delivered frame owns pixels after SDK frees input");
            const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            check(now-capture->GetLastFrameCaptureNs()<150000000,"newest delivered frame freshness");
        }
        if (sender) {
            sender->stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            capture->GetNextFrameCpu();
            sender->start();
            cv::Mat recovered;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(recovered.empty() && std::chrono::steady_clock::now()<deadline) {
                capture->WaitFrame(100); recovered=capture->GetNextFrameCpu();
            }
            check(!recovered.empty(),"same-source automatic reconnection after sender restart");
        }
        const auto shutdown=std::chrono::steady_clock::now(); capture.reset();
        check(std::chrono::steady_clock::now()-shutdown<std::chrono::seconds(1),"bounded shutdown");
        auto absent=ndi_capture::Create("Apotheosis non-existent source",64);
        check(absent && !absent->WaitFrame(100) && absent->GetNextFrameCpu().empty(),"must not substitute an unrelated source");
        std::cout << (desktop ? "DXGI desktop ROI readback, NDI receive and restart passed\n" :
            "NDI discovery, crop/color, latest frame, owned storage, source restart, missing source and shutdown passed\n");
        return 0;
    }catch(const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
