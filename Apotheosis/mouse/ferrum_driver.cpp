#define WIN32_LEAN_AND_MEAN
#include "ferrum_driver.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <sstream>
#include <algorithm>

namespace mouse_driver {
namespace {
bool finishPending(HANDLE serial, OVERLAPPED& operation, DWORD& transferred,
                   DWORD waitMs) {
    const DWORD wait = WaitForSingleObject(operation.hEvent, waitMs);
    if (wait == WAIT_OBJECT_0)
        return GetOverlappedResult(serial, &operation, &transferred, FALSE) != 0;
    CancelIoEx(serial, &operation);
    // The OVERLAPPED object and buffer must remain alive until cancellation
    // finishes, even when the virtual COM driver is slow to acknowledge it.
    return GetOverlappedResult(serial, &operation, &transferred, TRUE) != 0;
}

bool writeSerial(HANDLE serial, bool overlapped, const char* data, DWORD size,
                 DWORD& written, std::chrono::steady_clock::time_point& submittedAt) {
    written = 0;
    if (!overlapped) {
        const bool okay = WriteFile(serial, data, size, &written, nullptr) != 0;
        submittedAt = std::chrono::steady_clock::now();
        return okay && written == size;
    }
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    const BOOL started = WriteFile(serial, data, size, &written, &operation);
    const DWORD error = started ? ERROR_SUCCESS : GetLastError();
    submittedAt = std::chrono::steady_clock::now();
    const bool okay = started || (error == ERROR_IO_PENDING &&
        finishPending(serial, operation, written, 150));
    CloseHandle(operation.hEvent);
    return okay && written == size;
}

bool readAvailable(HANDLE serial, bool overlapped, char* data, DWORD capacity, DWORD& count) {
    count = 0;
    DWORD errors = 0;
    COMSTAT status{};
    if (!ClearCommError(serial, &errors, &status)) return false;
    if (status.cbInQue == 0) return true;
    const DWORD wanted = std::min(capacity, status.cbInQue);
    if (!overlapped) return ReadFile(serial, data, wanted, &count, nullptr) != 0;
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    const BOOL started = ReadFile(serial, data, wanted, &count, &operation);
    const DWORD error = started ? ERROR_SUCCESS : GetLastError();
    const bool okay = started || (error == ERROR_IO_PENDING &&
        finishPending(serial, operation, count, 20));
    CloseHandle(operation.hEvent);
    // A canceled read is not a disconnected device; the next poll retries.
    return okay || error == ERROR_IO_PENDING;
}
}

FerrumDriver::FerrumDriver(const std::string& port, unsigned int baud) {
    if (port.empty() || baud < 115200) { error_ = "Invalid Ferrum COM port/baud"; return; }
    const std::string path = port.rfind("\\\\.\\", 0) == 0 ? port : "\\\\.\\" + port;
    serial_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    overlapped_ = serial_ != INVALID_HANDLE_VALUE;
    if (!overlapped_)
        serial_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (serial_ == INVALID_HANDLE_VALUE) { error_ = "Cannot open Ferrum COM port"; return; }
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(serial_, &dcb)) { error_ = "GetCommState failed"; return; }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fAbortOnError = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(serial_, &dcb)) { error_ = "SetCommState failed"; return; }
    COMMTIMEOUTS timeouts{};
    // The reader asks for up to 128 bytes. A 20 ms blocking read can occupy
    // some virtual-COM bridges while a movement write is waiting to enter.
    // Poll only bytes already buffered; idle pacing happens in readLoop.
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutConstant = 100;
    if (!SetCommTimeouts(serial_, &timeouts)) { error_ = "SetCommTimeouts failed"; return; }
    SetupComm(serial_, 4096, 4096);
    PurgeComm(serial_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::string query = "km.version()\r\n";
    DWORD written = 0;
    auto submittedAt = std::chrono::steady_clock::now();
    if (!writeSerial(serial_, overlapped_, query.data(), static_cast<DWORD>(query.size()),
                     written, submittedAt)) { error_ = "Ferrum version query write failed"; return; }
    std::string reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (std::chrono::steady_clock::now() < deadline) {
        char data[128]; DWORD read = 0;
        if (readAvailable(serial_, overlapped_, data, sizeof(data), read) && read) {
            reply.append(data, read);
            if (reply.find("kmbox: Ferrum") != std::string::npos ||
                reply.find("kmbox: 2.0.0 Aug 31 2020 21:49:51") != std::string::npos) break;
            if (reply.size() > 4096) reply.erase(0, reply.size() - 4096);
        }
        if (!read) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    softwareApi_=reply.find("kmbox: Ferrum") != std::string::npos;
    if (!softwareApi_ && reply.find("kmbox: 2.0.0 Aug 31 2020 21:49:51") == std::string::npos) {
        error_ = "Ferrum version handshake timed out; check COM port and baud";
        return;
    }
    open_ = true;
    reader_ = std::thread(&FerrumDriver::readLoop, this);
    send("km.buttons(1)");
    if(softwareApi_) send("km.keys(1)");
}

FerrumDriver::~FerrumDriver() {
    for(int b=1;b<=5;++b) if(ownedButtonMasks_[b]) maskPhysicalButton(b,false);
    for(int a=0;a<2;++a) if(ownedAxisMasks_[a]) maskPhysicalAxis(a,false);
    for(int k=1;k<256;++k) if(ownedKeyMasks_[k]) maskPhysicalKey(k,false);
    for(int k=1;k<256;++k) if(injectedKeys_[k]) keyUp(k);
    if(isOpen() && softwareApi_) send("km.keys(0)");
    if (isOpen()) send("km.buttons(0)");
    stop_ = true;
    if (reader_.joinable()) reader_.join();
    open_ = false;
    if (serial_ != INVALID_HANDLE_VALUE) CloseHandle(serial_);
}

bool FerrumDriver::send(const std::string& command) {
    if (!isOpen()) return false;
    const std::string wire = command + "\r\n";
    const auto queuedAt = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(writeMutex_);
    const auto writeAt = std::chrono::steady_clock::now();
    DWORD written = 0;
    auto submittedAt = writeAt;
    const bool okay = writeSerial(serial_, overlapped_, wire.data(),
                                   static_cast<DWORD>(wire.size()), written, submittedAt);
    const auto doneAt = std::chrono::steady_clock::now();
    const auto writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(doneAt - writeAt).count();
    if (writeMs >= 10) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            doneAt.time_since_epoch()).count();
        auto lastMs = lastSlowWriteLogMs_.load();
        if (nowMs - lastMs >= 1000 && lastSlowWriteLogMs_.compare_exchange_strong(lastMs, nowMs)) {
            const auto lockMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                writeAt - queuedAt).count();
            std::cerr << "[Ferrum] Slow serial write: lock=" << lockMs
                      << "ms, WriteFile=" << writeMs << "ms, submit="
                      << std::chrono::duration<double, std::milli>(submittedAt - writeAt).count()
                      << "ms, mode=" << (overlapped_ ? "overlapped" : "synchronous")
                      << std::endl;
        }
    }
    if (!okay) open_ = false;
    return okay;
}

bool FerrumDriver::move(int dx, int dy) {
    return send("km.move(" + std::to_string(dx) + "," + std::to_string(dy) + ")");
}

bool FerrumDriver::button(int b, bool down) {
    static const char* names[]={"","left","right","middle","side1","side2"};
    if(b<1 || b>5) return false;
    return send(std::string("km.")+names[b]+(down ? "(1)" : "(0)"));
}
bool FerrumDriver::tapKey(int hid,int holdMs,int modifiers) {
    bool ok=true;
    for(int i=0;i<8;++i) if(modifiers & (1<<i)) ok=keyDown(224+i) && ok;
    ok=keyDown(hid) && ok;
    if(ok) std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(holdMs,1,500)));
    ok=keyUp(hid) && ok;
    for(int i=0;i<8;++i) if(modifiers & (1<<i)) ok=keyUp(224+i) && ok;
    return ok;
}
bool FerrumDriver::keyDown(int hid) {
    if(!softwareApi_ || hid<=0 || hid>=256) return false;
    injectedKeys_[hid]=true;
    return send("km.down("+std::to_string(hid)+")");
}
bool FerrumDriver::keyUp(int hid) {
    if(!softwareApi_ || hid<=0 || hid>=256) return false;
    const bool ok=send("km.up("+std::to_string(hid)+")");
    if(ok) injectedKeys_[hid]=false;
    return ok;
}

bool FerrumDriver::wheel(int delta) {
    return send("km.wheel(" + std::to_string(delta) + ")");
}

bool FerrumDriver::maskPhysicalAxis(int axis, bool enabled) {
    if(axis<0 || axis>1) return false;
    if(!enabled && !ownedAxisMasks_[axis]) return true;
    if(enabled) ownedAxisMasks_[axis]=true;
    const bool ok=send(std::string("km.lock_m")+(axis==0 ? "x" : "y")+(enabled ? "(1)" : "(0)"));
    if(ok && !enabled) ownedAxisMasks_[axis]=false;
    return ok;
}

bool FerrumDriver::maskPhysicalButton(int button, bool enabled) {
    static const char* names[]={"","ml","mr","mm","ms1","ms2"};
    if(button<1 || button>5) return false;
    if(enabled) ownedButtonMasks_[button]=true;
    const bool ok=send(std::string("km.lock_")+names[button]+(enabled ? "(1)" : "(0)"));
    if(ok && !enabled) ownedButtonMasks_[button]=false;
    return ok;
}
bool FerrumDriver::maskPhysicalKey(int hid, bool enabled) {
    if(!softwareApi_ || hid<=0 || hid>=256) return false;
    if(enabled) ownedKeyMasks_[hid]=true;
    const bool ok=send("km.mask("+std::to_string(hid)+(enabled ? ",1)" : ",0)"));
    if(ok && !enabled) ownedKeyMasks_[hid]=false;
    return ok;
}
int FerrumDriver::physicalKeyPressed(int hid) const {
    return hid>0 && hid<256 && keysReady_.load() ? static_cast<int>(physicalKeys_[hid].load()) : -1;
}

void FerrumDriver::readLoop() {
    int state = 0;
    std::string line;
    while (!stop_) {
        char data[128]; DWORD count = 0;
        if (!readAvailable(serial_, overlapped_, data, sizeof(data), count)) { open_ = false; break; }
        if (count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        for (DWORD i = 0; i < count; ++i) {
            const unsigned char c = static_cast<unsigned char>(data[i]);
            if(c=='\n') {
                const auto start=line.find("Keys(");
                const auto end=line.find(')',start==std::string::npos ? 0 : start);
                if(start!=std::string::npos && end!=std::string::npos) {
                    std::array<bool,256> keys{};
                    auto list=line.substr(start+5,end-start-5);
                    std::replace(list.begin(),list.end(),',',' ');
                    std::istringstream input(list); int key;
                    while(input>>key) if(key>0 && key<256) keys[key]=true;
                    for(int k=0;k<256;++k) physicalKeys_[k]=keys[k];
                    keysReady_=true;
                }
                line.clear();
            } else if(c>=32 && c<127) {
                if(line.size()<1024) line.push_back(static_cast<char>(c)); else line.clear();
            }
            switch (state) {
            case 0: state = c == 'k' ? 1 : 0; break;
            case 1: state = c == 'm' ? 2 : c == 'k' ? 1 : 0; break;
            case 2: state = c == '.' ? 3 : c == 'k' ? 1 : 0; break;
            case 3:
                if (c <= 31) buttons_ = static_cast<int>(c & 31);
                state = c == 'k' ? 1 : 0;
                break;
            }
        }
    }
}

int FerrumDriver::physicalButtonPressed(int button) const {
    const int bits = buttons_.load();
    return bits < 0 || button < 1 || button > 5 ? -1 : ((bits >> (button - 1)) & 1);
}
}
