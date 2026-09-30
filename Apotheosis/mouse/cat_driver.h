#pragma once
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include "mouse_driver.h"
#include <atomic>
#include <bitset>
#include <mutex>
#include <thread>

namespace mouse_driver {
// Wire protocol from the user's cat_net SDK (2025-02-11).
class CatDriver final : public IDriver {
public:
    CatDriver(const std::string& ip, unsigned short port, const std::string& uuid,
              unsigned short monitorPort);
    ~CatDriver() override;
    const char* name() const override { return "CAT"; }
    uint32_t capabilities() const override {
        return kCapMove | kCapButtonLeft | kCapButtonRight | kCapButtonMiddle |
               kCapButtonSide | kCapKeyboard | kCapPhysicalRead;
    }
    bool isOpen() const override { return open_.load(); }
    std::string lastError() const override { return error_; }
    bool move(int x, int y) override;
    bool button(int button, bool down) override;
    bool leftDown() override { return button(1,true); }
    bool leftUp() override { return button(1,false); }
    bool rightDown() override { return button(2,true); }
    bool rightUp() override { return button(2,false); }
    bool middleDown() override { return button(3,true); }
    bool middleUp() override { return button(3,false); }
    bool keyDown(int hid) override;
    bool tapKey(int hid, int holdMs, int modifiers=0) override;
    bool keyUp(int hid) override;
    int physicalButtonPressed(int button) const override;
    int physicalKeyPressed(int hid) const override;
    bool maskPhysicalButton(int button, bool enabled) override;
    bool maskPhysicalKey(int hid, bool enabled) override;
    bool directSend() const override { return false; }
private:
    bool command(uint8_t cmd, uint16_t options=0, int16_t v1=0, int16_t v2=0, int timeoutMs=100);
    bool crypt(bool encrypt, const uint8_t* input, int length, std::vector<uint8_t>& output);
    void readLoop();
    static int linuxKey(int hid);
    static int mouseCode(int button);
    SOCKET socket_=INVALID_SOCKET, monitor_=INVALID_SOCKET;
    sockaddr_in endpoint_{};
    BCRYPT_ALG_HANDLE algorithm_=nullptr;
    BCRYPT_KEY_HANDLE key_=nullptr;
    std::vector<uint8_t> keyObject_;
    bool wsa_=false;
    std::atomic<bool> open_{false}, stopping_{false}, monitorReady_{false};
    std::array<std::atomic<bool>,768> physical_{};
    std::thread reader_;
    std::mutex ioMutex_, cryptoMutex_;
    std::bitset<6> ownedButtonMasks_, injectedButtons_;
    std::bitset<256> ownedKeyMasks_, injectedKeys_;
    std::string error_;
};
}
