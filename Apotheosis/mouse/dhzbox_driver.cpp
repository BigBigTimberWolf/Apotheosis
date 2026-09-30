#define WIN32_LEAN_AND_MEAN
#include "dhzbox_driver.h"
#include <ws2tcpip.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace mouse_driver {
std::string DhzboxMiniDriver::encode(const std::string& command, int key) {
    std::string out = command;
    const int shift = ((key % 26) + 26) % 26;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>('a' + (c - 'a' + shift) % 26);
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>('A' + (c - 'A' + shift) % 26);
    }
    return out;
}

DhzboxMiniDriver::DhzboxMiniDriver(const std::string& ip, unsigned short port, int key) : key_(key) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { error_ = "WSAStartup failed"; return; }
    endpoint_.sin_family = AF_INET;
    endpoint_.sin_port = htons(port);
    if (!port || inet_pton(AF_INET, ip.c_str(), &endpoint_.sin_addr) != 1) {
        error_ = "Invalid DHZBox Mini IP/port";
        return;
    }
    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) { error_ = "UDP socket failed"; return; }
    monitor_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (monitor_ == INVALID_SOCKET) { error_ = "Monitor socket failed"; return; }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (bind(monitor_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        closesocket(monitor_); monitor_ = INVALID_SOCKET;
        error_ = "Monitor bind failed; mouse hotkeys use local state";
        return;
    }
    int len = sizeof(local);
    if (getsockname(monitor_, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
        send("monitor(" + std::to_string(ntohs(local.sin_port)) + ")");
        monitorThread_ = std::thread(&DhzboxMiniDriver::monitorLoop, this);
    }
}

DhzboxMiniDriver::~DhzboxMiniDriver() {
    for(int b=1;b<=5;++b) if(ownedMasks_[b]) maskPhysicalButton(b,false);
    for(int a=0;a<2;++a) if(ownedAxisMasks_[a]) maskPhysicalAxis(a,false);
    if (socket_ != INVALID_SOCKET) send("monitor(0)");
    stop_ = true;
    if (monitorThread_.joinable()) monitorThread_.join();
    if (monitor_ != INVALID_SOCKET) { closesocket(monitor_); monitor_ = INVALID_SOCKET; }
    if (socket_ != INVALID_SOCKET) closesocket(socket_);
    WSACleanup();
}

uint32_t DhzboxMiniDriver::capabilities() const {
    uint32_t c = kCapMove | kCapButtonLeft | kCapButtonRight | kCapButtonMiddle | kCapWheel;
    if (monitor_ != INVALID_SOCKET) c |= kCapPhysicalRead | kCapButtonSide;
    return c;
}

bool DhzboxMiniDriver::send(const std::string& command) {
    if (!isOpen()) return false;
    const std::string wire = encode(command, key_);
    std::lock_guard<std::mutex> lock(sendMutex_);
    return sendto(socket_, wire.data(), static_cast<int>(wire.size()), 0,
                  reinterpret_cast<const sockaddr*>(&endpoint_), sizeof(endpoint_)) == static_cast<int>(wire.size());
}

bool DhzboxMiniDriver::move(int dx, int dy) {
    while (dx || dy) {
        const int x = std::clamp(dx, -127, 127), y = std::clamp(dy, -127, 127);
        if (!send("move(" + std::to_string(x) + "," + std::to_string(y) + ")")) return false;
        dx -= x; dy -= y;
        if (dx || dy) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool DhzboxMiniDriver::button(int b,bool down) {
    static const char* names[]={"","left","right","middle","side1","side2"};
    return b>=1 && b<=5 && send(std::string(names[b])+(down ? "(1)" : "(0)"));
}

bool DhzboxMiniDriver::wheel(int delta) { return send("wheel(" + std::to_string(delta) + ")"); }

bool DhzboxMiniDriver::maskPhysicalAxis(int axis, bool enabled) {
    if(axis<0 || axis>1) return false;
    if(!enabled && !ownedAxisMasks_[axis]) return true;
    if(enabled) ownedAxisMasks_[axis]=true;
    const bool ok=send(std::string("mask_")+(axis==0 ? "x" : "y")+(enabled ? "(1)" : "(0)"));
    if(ok && !enabled) ownedAxisMasks_[axis]=false;
    return ok;
}

bool DhzboxMiniDriver::maskPhysicalButton(int button, bool enabled) {
    static const char* names[]={"","left","right","middle","side1","side2"};
    if(button<1 || button>5) return false;
    if(enabled) ownedMasks_[button]=true; // Retain uncertain writes for cleanup.
    const bool ok=send(std::string("mask_")+names[button]+(enabled ? "(1)" : "(0)"));
    if(ok && !enabled) ownedMasks_[button]=false;
    return ok;
}

void DhzboxMiniDriver::monitorLoop() {
    DWORD timeout = 100;
    setsockopt(monitor_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    char data[96];
    while (!stop_) {
        const int n = recv(monitor_, data, sizeof(data) - 1, 0);
        if (n <= 0) continue;
        data[n] = 0;
        int left, middle, right, side1, side2;
        if (std::sscanf(data, "%d|%d|%d|%d|%d|", &left, &middle, &right, &side1, &side2) == 5) {
            buttons_ = (!!left) | ((!!right) << 1) | ((!!middle) << 2) |
                       ((!!side1) << 3) | ((!!side2) << 4);
        }
    }
}

int DhzboxMiniDriver::physicalButtonPressed(int button) const {
    const int bits = buttons_.load();
    return (bits < 0 || button < 1 || button > 5) ? -1 : (bits >> (button - 1)) & 1;
}
}
