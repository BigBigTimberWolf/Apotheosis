#include "cat_driver.h"
#include <ws2tcpip.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace mouse_driver {
namespace {
#pragma pack(push,1)
struct Command { uint8_t cmd; uint16_t options; int16_t v1,v2; };
struct HidPacket { uint16_t mouseCode, mouseValue; int16_t x,y,wheel;
                   uint16_t keyCode,keyValue; uint8_t lock; };
#pragma pack(pop)
static_assert(sizeof(Command)==7 && sizeof(HidPacket)==15);
}
CatDriver::CatDriver(const std::string& ip, unsigned short port,
                     const std::string& uuid, unsigned short monitorPort) {
    std::string hex = uuid;
    hex.erase(hex.begin(), std::find_if(hex.begin(), hex.end(), [](unsigned char c) { return !std::isspace(c); }));
    hex.erase(std::find_if(hex.rbegin(), hex.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), hex.end());
    if(hex.size()>2 && hex[0]=='0' && (hex[1]=='x' || hex[1]=='X')) hex.erase(0,2);
    if(hex.empty() || hex.size()>8 ||
       !std::all_of(hex.begin(),hex.end(),[](unsigned char c){return std::isxdigit(c)!=0;}) ||
       !port || !monitorPort || port==monitorPort) {
        error_="CAT requires a 1-8 digit hexadecimal UUID and distinct command/monitor ports"; return;
    }
    // The supplied SDK parses UUID as a number, then prints eight lowercase
    // hexadecimal digits before padding the AES key with ASCII zeroes.
    std::ostringstream formatted;
    formatted << std::hex << std::setfill('0') << std::setw(8) << std::stoul(hex,nullptr,16);
    WSADATA w{};
    if(WSAStartup(MAKEWORD(2,2),&w)!=0) { error_="CAT WSAStartup failed"; return; }
    wsa_=true;
    endpoint_.sin_family=AF_INET; endpoint_.sin_port=htons(port);
    if(inet_pton(AF_INET,ip.c_str(),&endpoint_.sin_addr)!=1) { error_="Invalid CAT IPv4 address"; return; }
    std::array<uint8_t,16> secret{};
    secret.fill('0');
    const std::string normalized=formatted.str();
    for(size_t i=0;i<8;++i) secret[i]=static_cast<uint8_t>(normalized[i]);
    DWORD objectSize=0, got=0;
    if(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_AES_ALGORITHM,nullptr,0)<0 ||
       BCryptSetProperty(algorithm_,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),sizeof(BCRYPT_CHAIN_MODE_CBC),0)<0 ||
       BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&got,0)<0) {
        error_="CAT AES initialization failed"; return;
    }
    keyObject_.resize(objectSize);
    if(BCryptGenerateSymmetricKey(algorithm_,&key_,keyObject_.data(),objectSize,secret.data(),16,0)<0) {
        error_="CAT AES key initialization failed"; return;
    }
    socket_=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    monitor_=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    sockaddr_in local{}; local.sin_family=AF_INET; local.sin_port=htons(port);
    if(socket_==INVALID_SOCKET || monitor_==INVALID_SOCKET) {
        error_="CAT UDP socket creation failed (Windows error " + std::to_string(WSAGetLastError()) + ")"; return;
    }
    if(bind(socket_,reinterpret_cast<sockaddr*>(&local),sizeof(local))!=0) {
        error_="CAT command UDP port " + std::to_string(port) + " unavailable (Windows error " +
            std::to_string(WSAGetLastError()) + ")"; return;
    }
    local.sin_port=htons(monitorPort);
    if(bind(monitor_,reinterpret_cast<sockaddr*>(&local),sizeof(local))!=0) {
        error_="CAT monitor UDP port " + std::to_string(monitorPort) + " unavailable (Windows error " +
            std::to_string(WSAGetLastError()) + ")"; return;
    }
    DWORD timeout=100;
    setsockopt(monitor_,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    if(!command(1,0,0,0,5000)) { error_="CAT handshake timed out; check box IP, command port, UUID and firewall"; return; }
    if(!command(2,monitorPort,0,0,5000)) { error_="CAT monitor subscription timed out; check local monitor port and firewall"; return; }
    open_=true;
    reader_=std::thread(&CatDriver::readLoop,this);
}
CatDriver::~CatDriver() {
    if(isOpen()) {
        for(int b=1;b<=5;++b) {
            if(injectedButtons_[b]) button(b,false);
            if(ownedButtonMasks_[b] && !maskPhysicalButton(b,false)) maskPhysicalButton(b,false);
        }
        for(int k=1;k<256;++k) {
            if(injectedKeys_[k]) keyUp(k);
            if(ownedKeyMasks_[k] && !maskPhysicalKey(k,false)) maskPhysicalKey(k,false);
        }
        // The supplied SDK only closes its local monitor socket; no invented unsubscribe command.
    }
    open_=false; stopping_=true;
    if(reader_.joinable()) reader_.join();
    if(monitor_!=INVALID_SOCKET) closesocket(monitor_);
    if(socket_!=INVALID_SOCKET) closesocket(socket_);
    if(key_) BCryptDestroyKey(key_);
    if(algorithm_) BCryptCloseAlgorithmProvider(algorithm_,0);
    if(wsa_) WSACleanup();
}
bool CatDriver::crypt(bool encrypt,const uint8_t* input,int length,std::vector<uint8_t>& output) {
    std::lock_guard<std::mutex> lock(cryptoMutex_);
    if(!key_ || length<=0 || (!encrypt && (length<32 || length%16))) return false;
    std::array<uint8_t,16> iv{};
    ULONG size=0;
    if(encrypt) {
        if(BCryptGenRandom(nullptr,iv.data(),16,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) return false;
        output.resize(16+length+16);
        std::copy(iv.begin(),iv.end(),output.begin());
        if(BCryptEncrypt(key_,const_cast<PUCHAR>(input),length,nullptr,iv.data(),16,
                         output.data()+16,static_cast<ULONG>(output.size()-16),&size,BCRYPT_BLOCK_PADDING)<0) return false;
        output.resize(16+size);
    } else {
        std::copy(input,input+16,iv.begin()); output.resize(length-16);
        if(BCryptDecrypt(key_,const_cast<PUCHAR>(input+16),length-16,nullptr,iv.data(),16,
                         output.data(),static_cast<ULONG>(output.size()),&size,BCRYPT_BLOCK_PADDING)<0) return false;
        output.resize(size);
    }
    return true;
}
bool CatDriver::command(uint8_t cmd,uint16_t options,int16_t v1,int16_t v2,int timeoutMs) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if(socket_==INVALID_SOCKET) return false;
    Command request{cmd,options,v1,v2}; std::vector<uint8_t> wire;
    if(!crypt(true,reinterpret_cast<uint8_t*>(&request),sizeof(request),wire)) return false;
    char buffer[1024];
    // Discard old ACKs. Bound draining so a faulty peer cannot starve control.
    for(int i=0;i<32;++i) {
        fd_set fds; FD_ZERO(&fds); FD_SET(socket_,&fds); timeval tv{};
        if(select(0,&fds,nullptr,nullptr,&tv)<=0) break;
        recv(socket_,buffer,sizeof(buffer),0);
    }
    if(sendto(socket_,reinterpret_cast<const char*>(wire.data()),static_cast<int>(wire.size()),0,
              reinterpret_cast<const sockaddr*>(&endpoint_),sizeof(endpoint_))!=static_cast<int>(wire.size())) return false;
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
    while(std::chrono::steady_clock::now()<end) {
        auto us=std::chrono::duration_cast<std::chrono::microseconds>(end-std::chrono::steady_clock::now()).count();
        if(us<=0) break;
        fd_set fds; FD_ZERO(&fds); FD_SET(socket_,&fds);
        timeval tv{static_cast<long>(us/1000000),static_cast<long>(us%1000000)};
        if(select(0,&fds,nullptr,nullptr,&tv)<=0) break;
        sockaddr_in sender{}; int senderSize=sizeof(sender);
        const int n=recvfrom(socket_,buffer,sizeof(buffer),0,reinterpret_cast<sockaddr*>(&sender),&senderSize);
        if(n<=0 || sender.sin_addr.s_addr!=endpoint_.sin_addr.s_addr) continue;
        std::vector<uint8_t> plain;
        if(crypt(false,reinterpret_cast<uint8_t*>(buffer),n,plain) && plain.size()>=sizeof(Command) && plain[0]==cmd) {
            // The supplied SDK receives into box_endpoint, so a validated ACK
            // may update the peer's command port for subsequent requests.
            endpoint_.sin_port=sender.sin_port;
            return true;
        }
    }
    return false;
}
int CatDriver::mouseCode(int b) { return b>=1 && b<=5 ? 0x110+b-1 : -1; }
bool CatDriver::move(int x,int y) {
    return isOpen() && command(8,0,static_cast<int16_t>(std::clamp(x,-32768,32767)),static_cast<int16_t>(std::clamp(y,-32768,32767)));
}
bool CatDriver::button(int b,bool down) {
    const int code=mouseCode(b); if(!isOpen() || code<0) return false;
    if(down) injectedButtons_.set(b);
    const bool ok=command(3,static_cast<uint16_t>(code),down);
    if(ok && !down) injectedButtons_.reset(b);
    return ok;
}
int CatDriver::linuxKey(int hid) {
    static constexpr int letters[]={30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
    if(hid>=4 && hid<=29) return letters[hid-4];
    if(hid>=30 && hid<=38) return hid-28;
    if(hid>=58 && hid<=67) return hid+1;
    switch(hid) {
    case 39:return 11; case 40:return 28; case 41:return 1; case 42:return 14;
    case 43:return 15; case 44:return 57; case 45:return 12; case 46:return 13;
    case 47:return 26; case 48:return 27; case 49:return 43; case 50:return 43;
    case 51:return 39; case 52:return 40; case 53:return 41; case 54:return 51;
    case 55:return 52; case 56:return 53; case 57:return 58;
    case 68:return 87; case 69:return 88; case 70:return 99; case 71:return 70;
    case 72:return 119; case 73:return 110; case 74:return 102; case 75:return 104;
    case 76:return 111; case 77:return 107; case 78:return 109; case 79:return 106;
    case 80:return 105; case 81:return 108; case 82:return 103;
    case 83:return 69; case 84:return 98; case 85:return 55; case 86:return 74;
    case 87:return 78; case 88:return 96; case 89:return 79; case 90:return 80;
    case 91:return 81; case 92:return 75; case 93:return 76; case 94:return 77;
    case 95:return 71; case 96:return 72; case 97:return 73; case 98:return 82;
    case 99:return 83; case 100:return 86; case 101:return 127;
    case 224:return 29; case 225:return 42; case 226:return 56; case 227:return 125;
    case 228:return 97; case 229:return 54; case 230:return 100; case 231:return 126;
    default:return -1;
    }
}
bool CatDriver::tapKey(int hid,int holdMs,int modifiers) {
    bool ok=true;
    for(int i=0;i<8;++i) if(modifiers & (1<<i)) ok=keyDown(224+i) && ok;
    ok=keyDown(hid) && ok;
    if(ok) std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(holdMs,1,500)));
    ok=keyUp(hid) && ok;
    for(int i=0;i<8;++i) if(modifiers & (1<<i)) ok=keyUp(224+i) && ok;
    return ok;
}
bool CatDriver::keyDown(int hid) {
    const int code=linuxKey(hid); if(!isOpen() || code<0) return false;
    injectedKeys_.set(hid); return command(4,static_cast<uint16_t>(code),1);
}
bool CatDriver::keyUp(int hid) {
    const int code=linuxKey(hid); if(!isOpen() || code<0) return false;
    const bool ok=command(4,static_cast<uint16_t>(code),0);
    if(ok) injectedKeys_.reset(hid); return ok;
}
int CatDriver::physicalButtonPressed(int b) const {
    const int code=mouseCode(b); return monitorReady_ && code>=0 ? static_cast<int>(physical_[code].load()) : -1;
}
int CatDriver::physicalKeyPressed(int hid) const {
    const int code=linuxKey(hid); return monitorReady_ && code>=0 ? static_cast<int>(physical_[code].load()) : -1;
}
bool CatDriver::maskPhysicalButton(int b,bool enabled) {
    const int code=mouseCode(b); if(!isOpen() || code<0) return false;
    if(!enabled && !ownedButtonMasks_[b]) return true;
    if(enabled) ownedButtonMasks_.set(b);
    const bool ok=command(5,1,static_cast<int16_t>(code),enabled);
    if(ok && !enabled) ownedButtonMasks_.reset(b); return ok;
}
bool CatDriver::maskPhysicalKey(int hid,bool enabled) {
    const int code=linuxKey(hid); if(!isOpen() || code<0) return false;
    if(!enabled && !ownedKeyMasks_[hid]) return true;
    if(enabled) ownedKeyMasks_.set(hid);
    const bool ok=command(5,2,static_cast<int16_t>(code),enabled);
    if(ok && !enabled) ownedKeyMasks_.reset(hid); return ok;
}
void CatDriver::readLoop() {
    uint8_t buffer[1024];
    while(!stopping_) {
        sockaddr_in sender{}; int size=sizeof(sender);
        const int n=recvfrom(monitor_,reinterpret_cast<char*>(buffer),sizeof(buffer),0,reinterpret_cast<sockaddr*>(&sender),&size);
        if(n<=0 || sender.sin_addr.s_addr!=endpoint_.sin_addr.s_addr) continue;
        std::vector<uint8_t> plain;
        if(!crypt(false,buffer,n,plain) || plain.size()<sizeof(HidPacket)) continue;
        HidPacket p{}; std::memcpy(&p,plain.data(),sizeof(p));
        if(p.mouseCode>=0x110 && p.mouseCode<=0x114 && p.mouseValue<=1) physical_[p.mouseCode]=p.mouseValue!=0;
        if(p.keyCode>0 && p.keyCode<physical_.size() && p.keyValue<=2) physical_[p.keyCode]=p.keyValue!=0;
        monitorReady_=true;
    }
}
}
