#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "makcu_proto.h"
#include "serial/serial.h"

class MakcuNewConnection
{
public:
    MakcuNewConnection(const std::string& port, unsigned int baudRate);
    ~MakcuNewConnection();

    bool isOpen() const;
    unsigned int baudRate() const { return baudRate_; }
    bool move(int x, int y);
    void click(int button);
    void press(int button);
    void release(int button);
    void wheel(int delta);
    bool tapKey(int hidKey, int holdMs, int mod = 0);
    void cancelMove();
    bool physicalButtonPressed(int button) const;

    // ---- 瞬时屏蔽真实输入 ----
    //
    // 发 ASCII 文本命令(不用新增二进制帧): 固件侧 fw_device 收到后经板间
    // UART 转发给 fw_host 执行, fw_host 在 mask 窗口内直接丢弃真实键鼠输入。
    //
    // 语义: 在 durationMs 内, 该硬件 C 口接入的真实键盘输入不进入被控机;
    // 本连接的注入照常生效。也就是"只屏蔽真实输入, 不注入任何东西"。
    // durationMs <= 0 等价于立即解除。
    //
    // 为什么走键盘那台硬件: 真实键盘插在它上面, 屏蔽窗口只对它的真实输入
    // 有意义。鼠标那台没有真实键盘, 调它等于空操作。
    bool mask(int durationMs);
    bool maskOff();

    // ---- 物理按键回读的保活 ----
    //
    // 固件在【按键状态变化时】才推一帧, 订阅丢失/单帧丢失后不会再补发,
    // 表现为"硬件连上了、能移动, 但热键读不到按下"。这里提供一个保活接口,
    // 由上层周期性调用: 距上次成功订阅超过 intervalMs 就重发一次订阅。
    // 返回 true 表示本次真的重发了订阅。
    bool keepButtonStreamAlive(int intervalMs);

    // 最近一次订阅是否得到固件 ack。false = 按键回读链路不可信。
    bool buttonStreamReady() const { return buttonStreamReady_.load(); }

private:
    bool openSerial(unsigned int baudRate);
    void startReader();
    void stopReader();
    void readerLoop();
    void consumeFrames();

    void feedAsciiByte(uint8_t b);
    bool probeAscii(const std::string& command,
                    const std::string& expect,
                    unsigned int timeoutMs);

    void sendDeadBaud(unsigned int baud);
    bool establishSession();
    void supervisorLoop();
    void tryReconnect();
    void subscribeAsync(bool on);
    bool writeAsciiLine(const char* command);
    void applyPhysicalButtons(uint8_t mask);
    bool initializeProtocolSession();

    bool sendFrame(uint8_t command, const uint8_t* payload, size_t length);
    size_t encodeFrameLocked(uint8_t* output, size_t capacity, uint8_t command,
                             const uint8_t* payload, size_t length);
    static uint8_t buttonBit(int button);

    serial::Serial serial_;
    std::string port_;
    unsigned int baudRate_{115200};
    std::atomic<bool> portOpen_{false};
    std::atomic<bool> open_{false};
    std::atomic<bool> stopping_{false};
    std::thread reader_;
    std::mutex writeMutex_;
    uint8_t sequence_{0};

    unsigned int requestedBaud_{115200};
    std::atomic<bool> wantOpen_{false};
    std::thread supervisor_;
    std::mutex reconnectMutex_;

    std::atomic<uint8_t> outputButtons_{0};
    std::atomic<uint8_t> realButtons_{0};
    std::atomic<uint8_t> injectedButtons_{0};

    // 按键回读保活: 最近一次成功发订阅的时刻 / 最近一次收到 0x84 或裸掩码的时刻。
    std::atomic<bool> buttonStreamReady_{false};
    std::atomic<int64_t> lastButtonSubscribeMs_{0};
    std::atomic<int64_t> lastButtonFrameMs_{0};
    std::atomic<bool> makcuButtonsModeOn_{false};

    std::mutex asciiMutex_;
    std::condition_variable asciiCv_;
    std::string asciiLine_;
    std::string asciiAccum_;
    uint64_t asciiSeq_{0};

    std::vector<uint8_t> receiveBuffer_;
    size_t receiveOffset_{0};
};
