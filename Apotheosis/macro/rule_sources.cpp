#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <Xinput.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include "rule_sources.h"
#include "mouse/windows_driver.h"
#include <opencv2/opencv.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <mmsystem.h>

namespace macros {
namespace {
using Clock=std::chrono::steady_clock;
int64_t ms() {return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();}
std::string utf8(const wchar_t* p) {return winrt::to_string(winrt::hstring(p));}
struct Sources {
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool quit=false,pending=false;
    uint64_t generation=0;
    std::atomic<bool> enabled{false};
    std::atomic<int64_t> lastSubmit{0};
    std::atomic<int> interval{200};
    int resolution=0;
    cv::Mat cpu;GpuImage gpu;
    int64_t captureNs=0,completed=0,submitted=0;
    std::vector<Condition> conditions;
    std::vector<Condition> systemConditions;
    std::map<std::string,Value> result;
    std::map<std::string,cv::Mat> templates,previous;
    std::map<std::string,std::filesystem::file_time_type> modified;
    Sources():worker([this]{loop();}){}
    ~Sources(){ {std::lock_guard<std::mutex> l(mutex);quit=true;}cv.notify_one();if(worker.joinable())worker.join(); }
    void loop() {
        winrt::Windows::Media::Ocr::OcrEngine ocr{nullptr};
        bool apartment=false;try {winrt::init_apartment(winrt::apartment_type::multi_threaded);apartment=true;ocr=winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();}catch(...){}
        uint64_t previousVersion=0;
        for(;;) {
            cv::Mat image;GpuImage owned;std::vector<Condition> work;uint64_t version;int64_t frameTime;int scale;
            {std::unique_lock<std::mutex> l(mutex);cv.wait(l,[&]{return quit||pending;});if(quit)break;
                image=std::move(cpu);owned=std::move(gpu);work=conditions;version=generation;scale=resolution;frameTime=captureNs>0?captureNs/1000000:submitted;pending=false;}
            if(previousVersion!=version){templates.clear();modified.clear();previous.clear();previousVersion=version;}
            std::map<std::string,Value> values;
            try {
                if(!owned.empty())owned.download(image);
                if(image.empty())throw std::runtime_error("No capture image");
                if(scale>0&&(image.cols!=scale||image.rows!=scale))cv::resize(image,image,cv::Size(scale,scale));
                for(const auto& c:work) {
                    const auto key=sensorKey(c);if(values.count(key))continue;
                    cv::Rect area(0,0,image.cols,image.rows);
                    if(c.region[2]>0&&c.region[3]>0)area&=cv::Rect(c.region[0],c.region[1],c.region[2],c.region[3]);
                    if(area.empty()){values[key]={false,0,{},u8"识别区域不在画面内"};continue;}
                    cv::Mat roi=image(area),bgr,gray;
                    if(roi.channels()==4)cv::cvtColor(roi,bgr,cv::COLOR_BGRA2BGR);
                    else if(roi.channels()==1)cv::cvtColor(roi,bgr,cv::COLOR_GRAY2BGR);else bgr=roi;
                    cv::cvtColor(bgr,gray,cv::COLOR_BGR2GRAY);
                    if(c.metric=="image.hit"||c.metric=="image.score") {
                        std::error_code ec;auto modifiedAt=std::filesystem::last_write_time(std::filesystem::u8path(c.text),ec);
                        if(ec){values[key]={false,0,{},u8"模板文件不存在："+c.text};continue;}
                        if(!templates.count(c.text)||modified[c.text]!=modifiedAt) {
                            std::ifstream file(std::filesystem::u8path(c.text),std::ios::binary);
                            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),{});
                            templates[c.text]=bytes.empty()?cv::Mat{}:cv::imdecode(bytes,cv::IMREAD_GRAYSCALE);modified[c.text]=modifiedAt;
                        }
                        const auto& pattern=templates[c.text];
                        if(pattern.empty()||pattern.cols>gray.cols||pattern.rows>gray.rows){values[key]={false,0,{},u8"模板无效或大于识别区域"};continue;}
                        cv::Scalar mean,deviation;cv::meanStdDev(pattern,mean,deviation);
                        cv::Mat scores;cv::matchTemplate(gray,pattern,scores,cv::TM_SQDIFF_NORMED);
                        double low;cv::minMaxLoc(scores,&low);const double score=std::clamp(1-low,0.,1.);
                        values[key]=Value::numeric(c.metric=="image.score"?score:score>=std::clamp(c.upper,0.,1.));
                        values["image.hit"]=Value::numeric(score>=std::clamp(c.upper,0.,1.));
                    } else if(c.metric=="ocr.text") {
                        if(!ocr){values[key]={false,0,{},u8"Windows 未安装可用的 OCR 语言包"};continue;}
                        cv::Mat bgra;cv::cvtColor(bgr,bgra,cv::COLOR_BGR2BGRA);
                        if(bgra.cols>int(winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension())||bgra.rows>int(winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension())) {
                            values[key]={false,0,{},u8"OCR 区域超过系统尺寸上限"};continue;}
                        winrt::Windows::Storage::Streams::DataWriter writer;
                        writer.WriteBytes(winrt::array_view<const uint8_t>(bgra.data,bgra.data+bgra.total()*4));
                        auto bitmap=winrt::Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(writer.DetachBuffer(),
                            winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,bgra.cols,bgra.rows,
                            winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Ignore);
                        auto text=winrt::to_string(ocr.RecognizeAsync(bitmap).get().Text());values[key]=Value::string(text);
                        values["ocr.hit"]=Value::numeric(!c.text.empty()?text.find(c.text)!=std::string::npos:!text.empty());
                    } else if(c.metric=="pixels.count") {
                        unsigned rgb=0;try{rgb=std::stoul(c.text,nullptr,16);}catch(...){values[key]={false,0,{},u8"颜色请填写十六进制 RRGGBB"};continue;}
                        const int tolerance=std::clamp(int(c.upper),0,255);cv::Mat mask;
                        const cv::Scalar color(rgb&255,(rgb>>8)&255,(rgb>>16)&255);
                        cv::inRange(bgr,color-cv::Scalar::all(tolerance),color+cv::Scalar::all(tolerance),mask);
                        values[key]=Value::numeric(cv::countNonZero(mask));
                    } else if(c.metric=="pixels.multi") {
                        bool hit=true;const auto points=split(c.text,';');if(points.empty())hit=false;
                        for(auto point:points) {
                            std::replace(point.begin(),point.end(),',',' ');std::istringstream in(point);int x,y;std::string hex;
                            if(!(in>>x>>y>>hex)||x<0||y<0||x>=bgr.cols||y>=bgr.rows){hit=false;break;}
                            unsigned rgb;try{rgb=std::stoul(hex,nullptr,16);}catch(...){hit=false;break;}
                            const auto p=bgr.at<cv::Vec3b>(y,x);const int tol=std::clamp(int(c.upper),0,255);
                            hit&=std::abs(int(p[0])-int(rgb&255))<=tol&&std::abs(int(p[1])-int((rgb>>8)&255))<=tol&&std::abs(int(p[2])-int((rgb>>16)&255))<=tol;
                        }values[key]=Value::numeric(hit);
                    } else if(c.metric=="pixel.value") {
                        const auto p=bgr.at<cv::Vec3b>(0,0);values[key]=Value::numeric((p[2]<<16)|(p[1]<<8)|p[0]);
                    } else if(c.metric=="image.change") {
                        auto& prev=previous[key];if(!prev.empty()&&prev.size()==gray.size()) {
                            cv::Mat difference;cv::absdiff(gray,prev,difference);values[key]=Value::numeric(cv::mean(difference)[0]/255.);
                        } else values[key]={false,0,{},u8"等待下一张画面"};gray.copyTo(prev);
                    }
                }
            } catch(const std::exception& e){for(const auto& c:work)values[sensorKey(c)]={false,0,{},e.what()};}
              catch(const winrt::hresult_error& e){for(const auto& c:work)values[sensorKey(c)]={false,0,{},winrt::to_string(e.message())};}
            {std::lock_guard<std::mutex> l(mutex);if(version==generation){result=std::move(values);completed=frameTime;}}
        }
        if(apartment)winrt::uninit_apartment();
    }
};
Sources& sources(){static Sources v;return v;}
bool visionMetric(const std::string& m){return m.rfind("image.",0)==0||m.rfind("pixel",0)==0||m=="ocr.text";}
struct Gamepad {
    std::mutex mutex;
    HMODULE library=LoadLibraryW(L"xinput1_4.dll");
    using Get=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);using Set=DWORD(WINAPI*)(DWORD,XINPUT_VIBRATION*);
    Get get=library?reinterpret_cast<Get>(GetProcAddress(library,"XInputGetState")):nullptr;
    Set set=library?reinterpret_cast<Set>(GetProcAddress(library,"XInputSetState")):nullptr;
    std::array<int64_t,4> stopAt{};
    ~Gamepad(){if(set)for(DWORD i=0;i<4;++i){XINPUT_VIBRATION off{};set(i,&off);}if(library)FreeLibrary(library);}
};
Gamepad& gamepad(){static Gamepad p;return p;}
}
bool visionRequested(){return sources().enabled.load();}
void configureSources(const std::vector<Program>& programs,bool enabled,int resolution) {
    auto& s=sources();std::vector<Condition> conditions,systemConditions;int interval=200;
    if(enabled)for(const auto& p:programs) {
        for(const auto& c:p.conditions){if(visionMetric(c.metric))conditions.push_back(c);if(c.metric=="window.exists"||c.metric=="process.exists")systemConditions.push_back(c);}
        const auto event=option(p,"event");
        if(event=="ocr_found"||event=="ocr_lost"||event=="image_found"||event=="image_lost"){
            const auto requested=eventVisionConditions(p,event.rfind("image",0)==0);conditions.insert(conditions.end(),requested.begin(),requested.end());}
        if(event=="window_found"||event=="process_found"){Condition c;c.metric=event=="window_found"?"window.exists":"process.exists";c.text=option(p,"pattern");systemConditions.push_back(c);}
        interval=std::min(interval,std::clamp(number(p,"vision_interval_ms",200),50,5000));
    }
    std::lock_guard<std::mutex> l(s.mutex);s.conditions=std::move(conditions);s.systemConditions=std::move(systemConditions);++s.generation;s.result.clear();s.completed=0;
    s.enabled=!s.conditions.empty();s.interval=interval;s.resolution=resolution;s.pending=false;s.cpu.release();s.gpu.release();s.lastSubmit=0;
}
void stopSourceEffects(){auto& p=gamepad();std::lock_guard<std::mutex> lock(p.mutex);if(p.set)for(DWORD i=0;i<4;++i)if(p.stopAt[i]){XINPUT_VIBRATION off{};p.set(i,&off);p.stopAt[i]=0;}PlaySoundW(nullptr,nullptr,0);}
void stopSources(){configureSources({},false);stopSourceEffects();}
void submitVisionFrame(const cv::Mat& image,int64_t ns) {
    auto& s=sources();const auto now=ms();if(!s.enabled||now-s.lastSubmit<s.interval)return;
    std::lock_guard<std::mutex> l(s.mutex);s.cpu=image.clone();s.gpu.release();s.captureNs=ns;s.submitted=now;s.pending=true;s.lastSubmit=now;s.cv.notify_one();
}
void submitVisionFrame(const GpuImage& image,int64_t ns) {
    auto& s=sources();const auto now=ms();if(!s.enabled||now-s.lastSubmit<s.interval)return;
    std::lock_guard<std::mutex> l(s.mutex);s.gpu=image;s.cpu.release();s.captureNs=ns;s.submitted=now;s.pending=true;s.lastSubmit=now;s.cv.notify_one();
}
static void readSystemSources(RuleSnapshot& snapshot) {
    POINT cursor{};if(GetCursorPos(&cursor)){snapshot.values["mouse.x"]=Value::numeric(cursor.x);snapshot.values["mouse.y"]=Value::numeric(cursor.y);}
    static int64_t lastSystem=0;static std::map<std::string,Value> system;
    if(snapshot.now-lastSystem>=250){lastSystem=snapshot.now;system.clear();
        wchar_t title[1024]{};const auto foreground=GetForegroundWindow();GetWindowTextW(foreground,title,1024);
        system["window.title"]=Value::string(utf8(title));
        DWORD pid=0;GetWindowThreadProcessId(foreground,&pid);auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if(process){wchar_t path[32768];DWORD size=32768;if(QueryFullProcessImageNameW(process,0,path,&size))system["process.name"]=Value::string(utf8(std::filesystem::path(path).filename().c_str()));CloseHandle(process);}
        std::string names;const auto handle=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
        if(handle!=INVALID_HANDLE_VALUE){PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);if(Process32FirstW(handle,&entry))do{names+=utf8(entry.szExeFile)+"\n";}while(Process32NextW(handle,&entry));CloseHandle(handle);system["process.list"]=Value::string(names);}
        std::string titles;EnumWindows([](HWND window,LPARAM data)->BOOL{if(IsWindowVisible(window)){wchar_t name[1024]{};GetWindowTextW(window,name,1024);if(*name)*reinterpret_cast<std::string*>(data)+=utf8(name)+"\n";}return TRUE;},reinterpret_cast<LPARAM>(&titles));
        system["window.list"]=Value::string(titles);
    }
    snapshot.values.insert(system.begin(),system.end());
    SYSTEMTIME time;GetLocalTime(&time);snapshot.values["clock.minute"]=Value::numeric(time.wHour*60+time.wMinute);
    auto& pad=gamepad();std::lock_guard<std::mutex> lock(pad.mutex);
    static int64_t lastPad=0;static std::map<std::string,Value> padValues;static std::set<std::string> padKeys;
    for(DWORD i=0;i<4;++i)if(pad.stopAt[i]&&snapshot.now>=pad.stopAt[i]){XINPUT_VIBRATION off{};if(pad.set)pad.set(i,&off);pad.stopAt[i]=0;}
    if(snapshot.now-lastPad<8){snapshot.values.insert(padValues.begin(),padValues.end());snapshot.keys.insert(padKeys.begin(),padKeys.end());return;}
    lastPad=snapshot.now;padValues.clear();padKeys.clear();
    if(pad.get)for(DWORD i=0;i<4;++i) {
        XINPUT_STATE state{};const std::string prefix="Pad"+std::to_string(i)+":";
        if(pad.stopAt[i]&&snapshot.now>=pad.stopAt[i]) {XINPUT_VIBRATION off{};if(pad.set)pad.set(i,&off);pad.stopAt[i]=0;}
        if(pad.get(i,&state)!=ERROR_SUCCESS)continue;
        const std::pair<WORD,const char*> buttons[]={{XINPUT_GAMEPAD_A,"A"},{XINPUT_GAMEPAD_B,"B"},{XINPUT_GAMEPAD_X,"X"},
            {XINPUT_GAMEPAD_Y,"Y"},{XINPUT_GAMEPAD_DPAD_UP,"Up"},{XINPUT_GAMEPAD_DPAD_DOWN,"Down"},
            {XINPUT_GAMEPAD_DPAD_LEFT,"Left"},{XINPUT_GAMEPAD_DPAD_RIGHT,"Right"},{XINPUT_GAMEPAD_START,"Start"},
            {XINPUT_GAMEPAD_BACK,"Back"},{XINPUT_GAMEPAD_LEFT_SHOULDER,"LB"},{XINPUT_GAMEPAD_RIGHT_SHOULDER,"RB"},
            {XINPUT_GAMEPAD_LEFT_THUMB,"LS"},{XINPUT_GAMEPAD_RIGHT_THUMB,"RS"}};
        for(const auto& b:buttons)if(state.Gamepad.wButtons&b.first)padKeys.insert(prefix+b.second);
        const std::pair<const char*,double> axes[]={{"LX",state.Gamepad.sThumbLX/32767.},{"LY",state.Gamepad.sThumbLY/32767.},
            {"RX",state.Gamepad.sThumbRX/32767.},{"RY",state.Gamepad.sThumbRY/32767.},
            {"LT",state.Gamepad.bLeftTrigger/255.},{"RT",state.Gamepad.bRightTrigger/255.}};
        for(const auto& axis:axes) {padValues[prefix+axis.first]=Value::numeric(std::clamp(axis.second,-1.,1.));
            if((std::string(axis.first)=="LT"||std::string(axis.first)=="RT")&&axis.second>.5)padKeys.insert(prefix+axis.first);}
    }
    snapshot.values.insert(padValues.begin(),padValues.end());snapshot.keys.insert(padKeys.begin(),padKeys.end());
}
namespace {
struct SystemSources {
    std::mutex mutex;std::condition_variable wake;bool quit=false;RuleSnapshot cached;std::thread worker;
    SystemSources():worker([this]{for(;;){RuleSnapshot next;next.now=ms();readSystemSources(next);
        std::unique_lock<std::mutex> l(mutex);cached=std::move(next);if(wake.wait_for(l,std::chrono::milliseconds(8),[&]{return quit;}))break;}}){}
    ~SystemSources(){{std::lock_guard<std::mutex> l(mutex);quit=true;}wake.notify_one();if(worker.joinable())worker.join();}
};
SystemSources& systemSources(){gamepad();static SystemSources s;return s;}
}
void appendSources(RuleSnapshot& snapshot) {
    std::vector<Condition> systemConditions;
    auto& s=sources();{std::lock_guard<std::mutex> l(s.mutex);
        systemConditions=s.systemConditions;
        if(s.completed>0&&snapshot.now-s.completed<std::max(1000,s.interval.load()*3))snapshot.values.insert(s.result.begin(),s.result.end());}
    auto& system=systemSources();std::lock_guard<std::mutex> l(system.mutex);
    if(snapshot.now-system.cached.now<1000){
        for(const auto& value:system.cached.values)if(value.first!="process.list"&&value.first!="window.list")snapshot.values.insert(value);
        for(const auto& condition:systemConditions)snapshot.values[sensorKey(condition)]=metric(condition,system.cached);
        snapshot.keys.insert(system.cached.keys.begin(),system.cached.keys.end());}
}
bool sourceAction(const Action& action,const RuleSnapshot& snapshot,std::string& error) {
    if(action.type==ActionType::Sound) {
        if(action.text.empty()){MessageBeep(MB_OK);return true;}
        return PlaySoundW(winrt::to_hstring(action.text).c_str(),nullptr,SND_FILENAME|SND_ASYNC|SND_NODEFAULT)!=0;
    }
    if(action.type==ActionType::Vibration) {
        auto& p=gamepad();std::lock_guard<std::mutex> lock(p.mutex);if(!p.set){error=u8"XInput 不可用";return false;}
        const int id=std::clamp(action.c,0,3);XINPUT_VIBRATION vibration{};
        vibration.wLeftMotorSpeed=WORD(std::clamp(action.a,0,100)*65535/100);
        vibration.wRightMotorSpeed=WORD(std::clamp(action.b,0,100)*65535/100);
        if(p.set(id,&vibration)!=ERROR_SUCCESS){error=u8"指定手柄未连接";return false;}
        p.stopAt[id]=snapshot.now+std::max(1,action.d);return true;
    }
    error=u8"未知系统动作";return false;
}
}
