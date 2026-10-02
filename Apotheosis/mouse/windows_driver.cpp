#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "windows_driver.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace mouse_driver {
namespace {
std::array<std::atomic<bool>,256> physical{};
std::atomic<int64_t> wheelUp{0},wheelDown{0};
std::array<std::atomic<bool>,256> blockedHotkeys{}, blockedPress{};
bool recordPhysical(int vk,bool down,bool enteringUi=false) {
    if(down && !physical[vk].load()) {
        DWORD process=0;
        GetWindowThreadProcessId(GetForegroundWindow(),&process);
        // The initial down owns the whole press, including repeat and release.
        // Never eat clicks/typing used to edit our own UI.
        blockedPress[vk]=blockedHotkeys[vk].load() && !enteringUi && process!=GetCurrentProcessId();
    }
    const bool block=blockedPress[vk].load();
    physical[vk]=down;
    if(!down) blockedPress[vk]=false;
    return block;
}
bool forwardedPhysical(int vk) {
    return physical[vk].load() && !blockedPress[vk].load();
}
constexpr ULONG_PTR inputTag=0x41504f54; // APOT: distinguish our output from remote/user input.
constexpr int mouseVk[]={0,VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
LRESULT CALLBACK keyboardHook(int code,WPARAM message,LPARAM data) {
    if(code==HC_ACTION) {
        const auto& k=*reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if(!((k.flags&LLKHF_INJECTED) && k.dwExtraInfo==inputTag) && k.vkCode<256) {
            const bool down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN;
            int vk=static_cast<int>(k.vkCode);
            if(vk==VK_CONTROL) vk=(k.flags&LLKHF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
            if(vk==VK_MENU) vk=(k.flags&LLKHF_EXTENDED) ? VK_RMENU : VK_LMENU;
            if(vk==VK_SHIFT) vk=k.scanCode==0x36 ? VK_RSHIFT : VK_LSHIFT;
            if(recordPhysical(vk,down)) return 1;
        }
    }
    return CallNextHookEx(nullptr,code,message,data);
}
LRESULT CALLBACK mouseHook(int code,WPARAM message,LPARAM data) {
    if(code==HC_ACTION) {
        const auto& m=*reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        int vk=0; bool down=false;
        if(!((m.flags&LLMHF_INJECTED) && m.dwExtraInfo==inputTag)) switch(message) {
        case WM_MOUSEWHEEL:
            if(static_cast<short>(HIWORD(m.mouseData))>0)++wheelUp;else ++wheelDown;break;
        case WM_LBUTTONDOWN: vk=VK_LBUTTON; down=true; break;
        case WM_LBUTTONUP: vk=VK_LBUTTON; break;
        case WM_RBUTTONDOWN: vk=VK_RBUTTON; down=true; break;
        case WM_RBUTTONUP: vk=VK_RBUTTON; break;
        case WM_MBUTTONDOWN: vk=VK_MBUTTON; down=true; break;
        case WM_MBUTTONUP: vk=VK_MBUTTON; break;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            vk=HIWORD(m.mouseData)==XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
            down=message==WM_XBUTTONDOWN; break;
        }
        if(vk) {
            DWORD targetProcess=0;
            if(down) GetWindowThreadProcessId(WindowFromPoint(m.pt),&targetProcess);
            if(recordPhysical(vk,down,targetProcess==GetCurrentProcessId())) return 1;
        }
    }
    return CallNextHookEx(nullptr,code,message,data);
}
class PhysicalMonitor {
public:
    bool ready=false;
    PhysicalMonitor() {
        std::promise<bool> promise; auto future=promise.get_future();
        thread_=std::thread([this,p=std::move(promise)]() mutable {
            MSG message{}; PeekMessageW(&message,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
            threadId_=GetCurrentThreadId();
            for(int i=0;i<256;++i) physical[i]=(GetAsyncKeyState(i)&0x8000)!=0;
            const auto module=GetModuleHandleW(nullptr);
            HHOOK keyboard=SetWindowsHookExW(WH_KEYBOARD_LL,keyboardHook,module,0);
            HHOOK mouse=SetWindowsHookExW(WH_MOUSE_LL,mouseHook,module,0);
            p.set_value(keyboard && mouse);
            if(keyboard && mouse) while(GetMessageW(&message,nullptr,0,0)>0) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if(mouse) UnhookWindowsHookEx(mouse);
            if(keyboard) UnhookWindowsHookEx(keyboard);
        });
        ready=future.get();
    }
    ~PhysicalMonitor() {
        if(ready) PostThreadMessageW(threadId_,WM_QUIT,0,0);
        if(thread_.joinable()) thread_.join();
    }
private:
    DWORD threadId_=0;
    std::thread thread_;
};
PhysicalMonitor& monitor() { static PhysicalMonitor value; return value; }
int virtualKey(int hid) {
    if(hid>0x10000&&hid<0x10100)return hid&255;
    if(hid>=4 && hid<=29) return 'A'+hid-4;
    if(hid>=30 && hid<=38) return '1'+hid-30;
    if(hid==39) return '0';
    if(hid>=58 && hid<=69) return VK_F1+hid-58;
    if(hid>=104 && hid<=115) return VK_F13+hid-104;
    if(hid>=89 && hid<=97) return VK_NUMPAD1+hid-89;
    if(hid>=224 && hid<=231) { const int mods[]={VK_LCONTROL,VK_LSHIFT,VK_LMENU,VK_LWIN,VK_RCONTROL,VK_RSHIFT,VK_RMENU,VK_RWIN}; return mods[hid-224]; }
    switch(hid) {
    case 40: case 88: return VK_RETURN; case 41:return VK_ESCAPE; case 42:return VK_BACK;
    case 43:return VK_TAB; case 44:return VK_SPACE; case 45:return VK_OEM_MINUS;
    case 46:return VK_OEM_PLUS; case 47:return VK_OEM_4; case 48:return VK_OEM_6;
    case 49:return VK_OEM_5; case 51:return VK_OEM_1; case 52:return VK_OEM_7;
    case 53:return VK_OEM_3; case 54:return VK_OEM_COMMA; case 55:return VK_OEM_PERIOD;
    case 56:return VK_OEM_2; case 57:return VK_CAPITAL; case 70:return VK_SNAPSHOT;
    case 71:return VK_SCROLL; case 72:return VK_PAUSE; case 73:return VK_INSERT;
    case 74:return VK_HOME; case 75:return VK_PRIOR; case 76:return VK_DELETE;
    case 77:return VK_END; case 78:return VK_NEXT; case 79:return VK_RIGHT;
    case 80:return VK_LEFT; case 81:return VK_DOWN; case 82:return VK_UP;
    case 83:return VK_NUMLOCK; case 84:return VK_DIVIDE; case 85:return VK_MULTIPLY;
    case 86:return VK_SUBTRACT; case 87:return VK_ADD; case 98:return VK_NUMPAD0;
    case 99:return VK_DECIMAL;case 101:return VK_APPS; default:return 0;
    }
}
} // namespace
bool windowsPhysicalKeyPressed(int vk) { return vk>0 && vk<256 && monitor().ready && physical[vk].load(); }
int64_t windowsWheelCounter(bool up) {monitor();return up?wheelUp.load():wheelDown.load();}
bool windowsMoveAbsolute(int x,int y) {
    INPUT input{};input.type=INPUT_MOUSE;input.mi.dwExtraInfo=inputTag;
    const int left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);
    input.mi.dx=LONG(std::clamp(double(x-left)/std::max(1,GetSystemMetrics(SM_CXVIRTUALSCREEN)-1),0.,1.)*65535);
    input.mi.dy=LONG(std::clamp(double(y-top)/std::max(1,GetSystemMetrics(SM_CYVIRTUALSCREEN)-1),0.,1.)*65535);
    input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
    return SendInput(1,&input,sizeof(input))==1;
}
bool windowsTypeText(const std::string& text) {
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);if(size<=0)return text.empty();
    std::wstring value(size,0);MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),value.data(),size);
    std::vector<INPUT> input;input.reserve(size*2);
    for(wchar_t c:value){INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wScan=c;i.ki.dwExtraInfo=inputTag;i.ki.dwFlags=KEYEVENTF_UNICODE;input.push_back(i);i.ki.dwFlags|=KEYEVENTF_KEYUP;input.push_back(i);}
    return SendInput(UINT(input.size()),input.data(),sizeof(INPUT))==input.size();
}
bool windowsSetBlockedHotkeys(const std::bitset<256>& keys) {
    if(keys.any() && !monitor().ready) return false;
    for(int i=0;i<256;++i) blockedHotkeys[i]=keys[i];
    return true;
}
WindowsDriver::WindowsDriver() { if(!monitor().ready) error_=u8"Windows 物理按键监听初始化失败"; }
WindowsDriver::~WindowsDriver() {
    const auto keys=heldKeys_, buttons=heldButtons_;
    for(int k:keys) keyUp(k);
    for(int b:buttons) button(b,false);
}
bool WindowsDriver::isOpen() const { return monitor().ready; }
uint32_t WindowsDriver::capabilities() const {
    return kCapMove|kCapButtonLeft|kCapButtonRight|kCapButtonMiddle|kCapButtonSide|kCapWheel|kCapKeyboard|kCapPhysicalRead;
}
std::string WindowsDriver::lastError() const { std::lock_guard<std::recursive_mutex> lock(mutex_); return error_; }
bool WindowsDriver::result(unsigned int sent) {
    if(sent==1) { error_.clear(); return true; }
    error_=u8"Windows SendInput 发送失败（错误码 "+std::to_string(GetLastError())+u8"）；请检查目标窗口权限和系统输入限制";
    return false;
}
bool WindowsDriver::move(int dx,int dy) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    INPUT in{}; in.type=INPUT_MOUSE; in.mi.dx=dx; in.mi.dy=dy; in.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_MOVE_NOCOALESCE;
    in.mi.dwExtraInfo=inputTag;
    return result(SendInput(1,&in,sizeof(in)));
}
bool WindowsDriver::button(int channel,bool down) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if(channel<1 || channel>5) return false;
    if(down==static_cast<bool>(heldButtons_.count(channel))) return true;
    if(!down && forwardedPhysical(mouseVk[channel])) {
        heldButtons_.erase(channel); return true;
    }
    INPUT in{}; in.type=INPUT_MOUSE;
    in.mi.dwExtraInfo=inputTag;
    const DWORD downs[]={0,MOUSEEVENTF_LEFTDOWN,MOUSEEVENTF_RIGHTDOWN,MOUSEEVENTF_MIDDLEDOWN,MOUSEEVENTF_XDOWN,MOUSEEVENTF_XDOWN};
    const DWORD ups[]={0,MOUSEEVENTF_LEFTUP,MOUSEEVENTF_RIGHTUP,MOUSEEVENTF_MIDDLEUP,MOUSEEVENTF_XUP,MOUSEEVENTF_XUP};
    in.mi.dwFlags=down ? downs[channel] : ups[channel];
    if(channel>=4) in.mi.mouseData=channel==4 ? XBUTTON1 : XBUTTON2;
    if(!result(SendInput(1,&in,sizeof(in)))) return false;
    if(down) heldButtons_.insert(channel); else heldButtons_.erase(channel);
    return true;
}
bool WindowsDriver::wheel(int delta) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    INPUT in{}; in.type=INPUT_MOUSE; in.mi.dwFlags=MOUSEEVENTF_WHEEL;
    in.mi.dwExtraInfo=inputTag;
    in.mi.mouseData=static_cast<DWORD>(std::clamp(delta,-127,127)*WHEEL_DELTA);
    return result(SendInput(1,&in,sizeof(in)));
}
bool WindowsDriver::key(int hid,bool down) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const int vk=virtualKey(hid); if(!vk) return false;
    if(down==static_cast<bool>(heldKeys_.count(hid))) return true;
    if(!down && forwardedPhysical(vk)) { heldKeys_.erase(hid); return true; }
    INPUT in{}; in.type=INPUT_KEYBOARD;
    in.ki.dwExtraInfo=inputTag;
    const UINT scan=MapVirtualKeyW(vk,MAPVK_VK_TO_VSC_EX);
    in.ki.wScan=static_cast<WORD>(scan&0xff); in.ki.dwFlags=KEYEVENTF_SCANCODE;
    if((scan&0xff00)==0xe000 || hid==88) in.ki.dwFlags|=KEYEVENTF_EXTENDEDKEY;
    if(hid>=0x10000||!scan){in.ki.wVk=WORD(vk);in.ki.wScan=0;in.ki.dwFlags=0;}
    if(!down) in.ki.dwFlags|=KEYEVENTF_KEYUP;
    if(!result(SendInput(1,&in,sizeof(in)))) return false;
    if(down) heldKeys_.insert(hid); else heldKeys_.erase(hid);
    return true;
}
bool WindowsDriver::keyDown(int hid) { return key(hid,true); }
bool WindowsDriver::keyUp(int hid) { return key(hid,false); }
bool WindowsDriver::tapKey(int hid,int holdMs,int mod) {
    std::vector<int> modifiers;
    for(int bit=0;bit<8;++bit) if(mod&(1<<bit)) {
        if(!keyDown(224+bit)) { for(int k:modifiers) keyUp(k); return false; }
        modifiers.push_back(224+bit);
    }
    const bool down=keyDown(hid);
    if(down) std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(holdMs,1,2000)));
    bool ok=keyUp(hid) && down;
    for(int k:modifiers) ok=keyUp(k) && ok;
    return ok;
}
int WindowsDriver::physicalButtonPressed(int button) const {
    return button>=1 && button<=5 ? static_cast<int>(windowsPhysicalKeyPressed(mouseVk[button])) : -1;
}
} // namespace mouse_driver
