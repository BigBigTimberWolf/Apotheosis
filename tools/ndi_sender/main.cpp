#include "ndi/ndi_runtime.h"
#include "ndi/latest_slot.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <timeapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
namespace {
std::atomic<bool> stopping{false};
BOOL WINAPI onConsole(DWORD) { stopping = true; return TRUE; }
void check(HRESULT hr, const char* action) {
    if (FAILED(hr)) throw std::runtime_error(std::string(action) + " failed, HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
}
struct Options {
    std::string name = "Apotheosis";
    int monitor = 0, side = 320, fps = 120, seconds = 0;
    bool list = false, pattern = false;
};
int number(const char* text, int low, int high) {
    size_t used = 0;
    const std::string value(text);
    const int n = std::stoi(value, &used);
    if (used != value.size() || n < low || n > high) throw std::runtime_error("Numeric argument out of range: " + value);
    return n;
}
Options parse(int argc, char** argv) {
    Options opts;
    for (int i=1; i<argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "ndi_sender [--name Apotheosis] [--monitor 0] [--size 320] [--fps 120]\n"
                         "           [--seconds N] [--list] [--test-pattern]\n"
                         "--size: center square, even 32..4096; 0 sends the entire monitor.\n"
                         "Default: center 320x320, up to 120 fps, no audio/cursor. Ctrl+C stops.\n";
            std::exit(0);
        } else if (arg == "--list") opts.list = true;
        else if (arg == "--test-pattern") opts.pattern = true;
        else {
            if (++i >= argc) throw std::runtime_error("Missing value for " + arg);
            if (arg == "--name") opts.name = argv[i];
            else if (arg == "--monitor") opts.monitor = number(argv[i], 0, 63);
            else if (arg == "--size") opts.side = number(argv[i], 0, 4096);
            else if (arg == "--fps") opts.fps = number(argv[i], 1, 240);
            else if (arg == "--seconds") opts.seconds = number(argv[i], 1, 86400);
            else throw std::runtime_error("Unknown option " + arg);
        }
    }
    if (opts.side != 0 && (opts.side < 32 || opts.side % 2)) throw std::runtime_error("--size must be 0 or an even number 32..4096");
    if (opts.name.empty() || opts.name.size() > 200 || opts.name.find_first_of("\\/:*?\"<>|\r\n") != std::string::npos)
        throw std::runtime_error("Invalid source name (1..200 UTF-8 bytes; no reserved characters)");
    return opts;
}
struct Monitor {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    DXGI_OUTPUT_DESC desc{};
};
std::vector<Monitor> monitors() {
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    std::vector<Monitor> result;
    for (UINT a=0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto hr = factory->EnumAdapters1(a, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        check(hr, "EnumAdapters1");
        for (UINT o=0;; ++o) {
            ComPtr<IDXGIOutput> output;
            const auto outHr = adapter->EnumOutputs(o, &output);
            if (outHr == DXGI_ERROR_NOT_FOUND) break;
            check(outHr, "EnumOutputs");
            Monitor m; m.adapter = adapter;
            check(output->GetDesc(&m.desc), "GetDesc");
            if (!m.desc.AttachedToDesktop) continue;
            check(output.As(&m.output), "IDXGIOutput1");
            result.push_back(std::move(m));
        }
    }
    return result;
}
struct Pixels {
    std::vector<uint8_t> bytes;
    int width=0, height=0;
    Clock::time_point captured;
};
using Buffer = std::shared_ptr<Pixels>;
class Desktop {
public:
    Desktop(const Options& opts) {
        auto displays = monitors();
        if (opts.monitor >= int(displays.size())) throw std::runtime_error("Monitor is unavailable; use --list");
        const auto& monitor = displays[opts.monitor];
        if (monitor.desc.Rotation != DXGI_MODE_ROTATION_IDENTITY && monitor.desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
            throw std::runtime_error("Rotated monitors are unsupported; select a landscape monitor");
        D3D_FEATURE_LEVEL level;
        check(D3D11CreateDevice(monitor.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device_, &level, &context_), "D3D11CreateDevice");
        check(monitor.output->DuplicateOutput(device_.Get(), &duplication_), "DuplicateOutput");
        DXGI_OUTDUPL_DESC desc; duplication_->GetDesc(&desc);
        if (desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) throw std::runtime_error("Desktop format is not BGRA8");
        width_ = opts.side ? std::min<int>(opts.side, std::min(desc.ModeDesc.Width, desc.ModeDesc.Height)) : desc.ModeDesc.Width;
        height_ = opts.side ? width_ : desc.ModeDesc.Height;
        width_ &= ~1; // NDI requires even width.
        if (width_ < 2 || height_ < 2) throw std::runtime_error("Invalid desktop size");
        x_ = (desc.ModeDesc.Width-width_)/2; y_ = (desc.ModeDesc.Height-height_)/2;
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width=width_; texture.Height=height_; texture.MipLevels=1; texture.ArraySize=1;
        texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM; texture.SampleDesc.Count=1;
        texture.Usage=D3D11_USAGE_STAGING; texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        for (auto& slot : staging_) check(device_->CreateTexture2D(&texture, nullptr, &slot.texture), "Create staging texture");
        for (auto& buffer : pool_) {
            buffer = std::make_shared<Pixels>(); buffer->width=width_; buffer->height=height_;
            buffer->bytes.resize(size_t(width_)*height_*4);
        }
        std::cout << "Desktop ROI " << width_ << "x" << height_ << ", FPS cap " << opts.fps << std::endl;
    }
    void capture() {
        Slot* free = nullptr;
        for (auto& slot : staging_) if (!slot.pending) { free=&slot; break; }
        if (!free) return; // bounded GPU in-flight storage; never wait for it
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const auto hr = duplication_->AcquireNextFrame(0, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
        check(hr, "AcquireNextFrame (desktop/session changed)");
        struct Release { IDXGIOutputDuplication* d; ~Release() { d->ReleaseFrame(); } } release{duplication_.Get()};
        if (!info.LastPresentTime.QuadPart) return; // cursor-only update
        ComPtr<ID3D11Texture2D> desktop;
        check(resource.As(&desktop), "Desktop texture");
        const D3D11_BOX roi{UINT(x_), UINT(y_), 0, UINT(x_+width_), UINT(y_+height_), 1};
        context_->CopySubresourceRegion(free->texture.Get(), 0, 0, 0, 0, desktop.Get(), 0, &roi);
        context_->Flush();
        // Age measures time held in our pipeline. A static desktop can have an
        // old LastPresentTime and still be the correct current image.
        free->captured = Clock::now();
        free->sequence = ++sequence_; free->pending = true;
    }
    Buffer readLatest() {
        Buffer buffer;
        for (auto& candidate : pool_) if (candidate.use_count()==1) { buffer=candidate; break; }
        if (!buffer) return {};
        // Try newest first. Map with DO_NOT_WAIT avoids stalls on the GPU.
        std::array<Slot*,3> order{&staging_[0],&staging_[1],&staging_[2]};
        std::sort(order.begin(),order.end(),[](auto* a, auto* b) { return a->sequence > b->sequence; });
        for (auto* slot : order) {
            if (!slot->pending) continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const auto hr = context_->Map(slot->texture.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            check(hr, "Map staging texture");
            for (int y=0;y<height_;++y)
                std::memcpy(buffer->bytes.data()+size_t(y)*width_*4, static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch, size_t(width_)*4);
            context_->Unmap(slot->texture.Get(),0);
            buffer->captured=slot->captured;
            // D3D immediate-context copies are ordered: all older slots are
            // complete when this one maps, and can be safely overwritten.
            for (auto& old : staging_) if (old.sequence <= slot->sequence) old.pending=false;
            return buffer;
        }
        return {};
    }
private:
    struct Slot { ComPtr<ID3D11Texture2D> texture; bool pending=false; uint64_t sequence=0; Clock::time_point captured; };
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    std::array<Slot,3> staging_;
    std::array<Buffer,4> pool_;
    int width_=0,height_=0,x_=0,y_=0;
    uint64_t sequence_=0;
};
struct Sender {
    std::shared_ptr<ndi::Runtime> rt;
    NDIlib_send_instance_t handle=nullptr;
    ~Sender() { if (handle) { rt->send_send_video_async_v2(handle,nullptr); rt->send_destroy(handle); } }
};
int run(const Options& opts) {
    if (opts.list) {
        const auto displays=monitors();
        for (size_t i=0;i<displays.size();++i) {
            const auto& r=displays[i].desc.DesktopCoordinates;
            std::wcout << i << L": " << displays[i].desc.DeviceName << L" " << r.right-r.left << L"x" << r.bottom-r.top << L"\n";
        }
        return 0;
    }
    std::string error;
    Sender sender; sender.rt=ndi::Runtime::Load(error);
    if (!sender.rt) throw std::runtime_error(error);
    NDIlib_send_create_t settings;
    settings.p_ndi_name=opts.name.c_str(); settings.clock_video=false; settings.clock_audio=false;
    sender.handle=sender.rt->send_create(&settings);
    if (!sender.handle) throw std::runtime_error("Cannot create NDI sender");
    const auto* source=sender.rt->send_get_source_name(sender.handle);
    std::cout << "NDI source: " << (source && source->p_ndi_name ? source->p_ndi_name : opts.name) << "\nCtrl+C stops.\n";
    ndi::LatestSlot<Buffer> frames;
    std::atomic<bool> connected{false};
    std::atomic<uint64_t> captured{0};
    std::atomic<bool> producerFailed{false};
    // Keep the previous async frame alive until the next send call returns.
    // Declare before the worker so cleanup can always flush before releasing it.
    Buffer inFlight;
    std::thread producer([&] {
        try {
            std::unique_ptr<Desktop> desktop;
            const auto interval=std::chrono::nanoseconds(1000000000LL/opts.fps);
            auto next=Clock::now();
            uint64_t tick=0;
            while (!stopping) {
                if (!connected) { desktop.reset(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); next=Clock::now(); continue; }
                if (opts.pattern) {
                    if (Clock::now()<next) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
                    next=Clock::now()+interval;
                    auto b=std::make_shared<Pixels>(); b->width=b->height=opts.side ? opts.side : 320;
                    b->bytes.resize(size_t(b->width)*b->height*4);
                    for (int y=0;y<b->height;++y) for (int x=0;x<b->width;++x) {
                        const size_t p=(size_t(y)*b->width+x)*4;
                        b->bytes[p]=uint8_t(tick); b->bytes[p+1]=uint8_t(y); b->bytes[p+2]=uint8_t(x); b->bytes[p+3]=255;
                    }
                    b->captured=Clock::now(); ++tick; ++captured; frames.publish(std::move(b)); continue;
                }
                try {
                    if (!desktop) desktop=std::make_unique<Desktop>(opts);
                    if (Clock::now()>=next) { desktop->capture(); next=Clock::now()+interval; }
                    if (auto b=desktop->readLatest()) { ++captured; frames.publish(std::move(b)); }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } catch (const std::exception& e) {
                    desktop.reset();
                    std::cerr << "Capture unavailable; retry in 1 second: " << e.what() << std::endl;
                    for (int i=0;i<100 && !stopping;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    next=Clock::now();
                }
            }
        } catch (const std::exception& e) { std::cerr << e.what() << std::endl; producerFailed=true; stopping=true; }
        frames.close();
    });
    // Join and synchronize even if a subsequent allocation/SDK call throws.
    struct Cleanup {
        std::thread& worker; ndi::LatestSlot<Buffer>& frames; Sender& sender; Buffer& inFlight;
        ~Cleanup() { stopping=true; frames.close(); if(worker.joinable()) worker.join();
            sender.rt->send_send_video_async_v2(sender.handle,nullptr); inFlight.reset(); }
    } cleanup{producer,frames,sender,inFlight};
    const auto started=Clock::now();
    auto lastStats=started;
    uint64_t sent=0, expired=0;
    double submitMs=0,maxSubmitMs=0,maxAgeMs=0;
    while (!stopping) {
        if (opts.seconds && Clock::now()-started>=std::chrono::seconds(opts.seconds)) break;
        const int connections=sender.rt->send_get_no_connections(sender.handle,0);
        connected=connections>0;
        if (!connected) { frames.take(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        else if (frames.wait(20)) {
            auto pending=frames.take();
            if (!pending) continue;
            Buffer next=std::move(*pending);
            const double ageMs=std::chrono::duration<double,std::milli>(Clock::now()-next->captured).count();
            if (ageMs>std::max(50.0,3000.0/opts.fps)) { ++expired; continue; }
            NDIlib_video_frame_v2_t video;
            video.xres=next->width; video.yres=next->height; video.FourCC=NDIlib_FourCC_video_type_BGRX;
            video.frame_rate_N=opts.fps; video.frame_rate_D=1;
            video.picture_aspect_ratio=float(next->width)/next->height;
            video.frame_format_type=NDIlib_frame_format_type_progressive;
            video.timecode=NDIlib_send_timecode_synthesize;
            video.p_data=next->bytes.data(); video.line_stride_in_bytes=next->width*4;
            const auto begin=Clock::now();
            sender.rt->send_send_video_async_v2(sender.handle,&video);
            inFlight=std::move(next); // previous storage may be reused only now
            const double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
            submitMs+=ms; maxSubmitMs=std::max(maxSubmitMs,ms); maxAgeMs=std::max(maxAgeMs,ageMs); ++sent;
        }
        const auto now=Clock::now();
        if(now-lastStats>=std::chrono::seconds(2)) {
            const double elapsed=std::chrono::duration<double>(now-lastStats).count();
            std::cout << "connections=" << connections << " capture=" << captured.exchange(0)/elapsed
                << "fps send=" << sent/elapsed << "fps expired=" << expired
                << " submit_avg=" << (sent ? submitMs/sent : 0) << "ms submit_max=" << maxSubmitMs
                << "ms capture_age_max=" << maxAgeMs << "ms\n";
            lastStats=now; sent=expired=0; submitMs=maxSubmitMs=maxAgeMs=0;
        }
    }
    return producerFailed ? 1 : 0;
}
}
int wmain(int argc,wchar_t** wideArgv) {
    try {
        std::vector<std::string> utf8Args;
        utf8Args.reserve(argc);
        for(int i=0;i<argc;++i) {
            const int length=int(std::wcslen(wideArgv[i]));
            const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wideArgv[i],length,nullptr,0,nullptr,nullptr);
            if(length && !n) throw std::runtime_error("Invalid Unicode argument");
            std::string value(n,'\0');
            if(n) WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wideArgv[i],length,value.data(),n,nullptr,nullptr);
            utf8Args.push_back(std::move(value));
        }
        std::vector<char*> argv;
        for(auto& value:utf8Args) argv.push_back(value.data());
        const auto opts=parse(argc,argv.data());
        SetConsoleCtrlHandler(onConsole,TRUE);
        timeBeginPeriod(1);
        struct Timer { ~Timer() { timeEndPeriod(1); } } timer;
        return run(opts);
    } catch(const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
