#include "MakcuNew.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
static void autoTuneCh343Latency(const std::string& portName)
{
    int portNum = 0;
    size_t pos = portName.find("COM");
    if (pos != std::string::npos) {
        portNum = std::atoi(portName.c_str() + pos + 3);
    }
    if (portNum <= 0) return;

    char regSubKey[256];
    snprintf(regSubKey, sizeof(regSubKey),
             "SYSTEM\\CurrentControlSet\\Services\\CH341SER\\Parameters");
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regSubKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD latencyVal = 1;
        RegSetValueExA(hKey, "LatencyTimer", 0, REG_DWORD,
                       (const BYTE*)&latencyVal, sizeof(latencyVal));
        RegCloseKey(hKey);
    }
}
#endif

namespace
{
constexpr size_t kMaxPayload = 244;
constexpr unsigned int kBootBaud = 115200;
constexpr const char* kVersionTag = "MAKCU-PASSTHROUGH";

uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
}

MakcuNewConnection::MakcuNewConnection(const std::string& port, unsigned int baudRate)
    : port_(port)
{
    requestedBaud_ = baudRate ? baudRate : kBootBaud;
    if (requestedBaud_ < 115200) requestedBaud_ = kBootBaud;
    if (requestedBaud_ > 6000000) requestedBaud_ = 6000000;
    baudRate_ = requestedBaud_;

#ifdef _WIN32
    autoTuneCh343Latency(port_);
#endif

    wantOpen_.store(true);

    if (!establishSession())
    {
        wantOpen_.store(false);
        stopReader();
        try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
        std::cerr << "[MakcuNew] Failed to establish passthrough session on "
                  << port_ << std::endl;
        portOpen_.store(false);
        open_.store(false);
    }
    else
    {
        supervisor_ = std::thread(&MakcuNewConnection::supervisorLoop, this);
    }
}

MakcuNewConnection::~MakcuNewConnection()
{
    wantOpen_.store(false);
    if (open_.load())
    {
        subscribeAsync(false);
    }
    if (supervisor_.joinable()) supervisor_.join();
    stopReader();
    try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
    portOpen_.store(false);
    open_.store(false);
}

bool MakcuNewConnection::establishSession()
{
    const unsigned int requestedBaud = requestedBaud_;

    const auto closeSession = [this] {
        stopReader();
        try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
        portOpen_.store(false);
        open_.store(false);
        receiveBuffer_.clear();
        receiveOffset_ = 0;
        std::lock_guard<std::mutex> lk(asciiMutex_);
        asciiAccum_.clear();
        asciiLine_.clear();
    };

    const auto tryConnectAt = [&](unsigned int baud, const char* note) -> bool {
        if (!openSerial(baud)) return false;
        startReader();
        if (!probeAscii("km.version", kVersionTag, 500))
        {
            closeSession();
            return false;
        }
        baudRate_ = baud;
        open_.store(true);
        initializeProtocolSession();
        std::cout << "[MakcuNew] Connected on " << port_ << " @ " << baudRate_
                  << " bps" << (note ? note : "") << std::endl;
        return true;
    };

    std::vector<unsigned int> candidates;
    if (requestedBaud != kBootBaud) candidates.push_back(kBootBaud);
    candidates.push_back(requestedBaud);

    try
    {
        for (unsigned int candidate : candidates)
        {
            if (!openSerial(candidate)) continue;
            startReader();

            const bool alive = probeAscii("km.version", kVersionTag, 500);
            if (!alive)
            {
                closeSession();
                continue;
            }

            if (candidate == requestedBaud)
            {
                baudRate_ = candidate;
                open_.store(true);
                initializeProtocolSession();
                std::cout << "[MakcuNew] Connected to MAKCU passthrough device on "
                          << port_ << " @ " << baudRate_ << " bps." << std::endl;
                return true;
            }

            std::cout << "[MakcuNew] Requesting baud switch to " << requestedBaud
                      << " bps ..." << std::endl;

            {
                uint8_t payload[4];
                payload[0] = static_cast<uint8_t>(requestedBaud & 0xFF);
                payload[1] = static_cast<uint8_t>((requestedBaud >> 8) & 0xFF);
                payload[2] = static_cast<uint8_t>((requestedBaud >> 16) & 0xFF);
                payload[3] = static_cast<uint8_t>((requestedBaud >> 24) & 0xFF);
                sendFrame(makcu::CMD_SET_BAUD, payload, sizeof(payload));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            closeSession();

            if (tryConnectAt(requestedBaud, " (SET_BAUD)")) return true;

            closeSession();
            if (openSerial(kBootBaud))
            {
                startReader();
                if (probeAscii("km.version", kVersionTag, 500))
                {
                    sendDeadBaud(requestedBaud);
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    closeSession();
                    if (tryConnectAt(requestedBaud, " (DE AD)")) return true;
                }
            }

            closeSession();
            if (tryConnectAt(kBootBaud, ""))
            {
                std::cerr << "[MakcuNew] Baud switch failed, fell back to 115200 bps."
                          << std::endl;
                return true;
            }
            closeSession();
            break;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MakcuNew] Connection error: " << e.what() << std::endl;
    }

    stopReader();
    try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
    portOpen_.store(false);
    open_.store(false);
    return false;
}

void MakcuNewConnection::supervisorLoop()
{
    while (wantOpen_.load())
    {
        for (int i = 0; i < 40 && wantOpen_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (!wantOpen_.load()) break;
        if (open_.load()) continue;
        tryReconnect();
    }
}

void MakcuNewConnection::tryReconnect()
{
    std::lock_guard<std::mutex> lk(reconnectMutex_);
    if (open_.load() || !wantOpen_.load()) return;

    std::cerr << "[MakcuNew] Link lost on " << port_ << ", reconnecting ..." << std::endl;
    stopReader();
    try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
    receiveBuffer_.clear();
    receiveOffset_ = 0;
    {
        std::lock_guard<std::mutex> al(asciiMutex_);
        asciiAccum_.clear();
        asciiLine_.clear();
    }

    if (establishSession())
        std::cout << "[MakcuNew] Reconnected on " << port_
                  << " @ " << baudRate_ << " bps." << std::endl;
}

bool MakcuNewConnection::isOpen() const
{
    return open_.load();
}

bool MakcuNewConnection::openSerial(unsigned int baudRate)
{
    try
    {
        if (serial_.isOpen()) serial_.close();
        serial_.setPort(port_);
        serial_.setBaudrate(baudRate);
        serial::Timeout timeout(5, 10, 0, 50, 0);
        serial_.setTimeout(timeout);
        serial_.open();
        const bool opened = serial_.isOpen();
        portOpen_.store(opened);
        if (opened)
        {
            std::lock_guard<std::mutex> lk(asciiMutex_);
            asciiAccum_.clear();
            asciiLine_.clear();
            asciiSeq_ = 0;
        }
        return opened;
    }
    catch (...)
    {
        portOpen_.store(false);
        open_.store(false);
        return false;
    }
}

void MakcuNewConnection::startReader()
{
    stopping_.store(false);
    if (!reader_.joinable())
        reader_ = std::thread(&MakcuNewConnection::readerLoop, this);
}

void MakcuNewConnection::stopReader()
{
    stopping_.store(true);
    if (reader_.joinable()) reader_.join();
}

size_t MakcuNewConnection::encodeFrameLocked(
    uint8_t* output, size_t capacity, uint8_t command,
    const uint8_t* payload, size_t length)
{
    const size_t frameLength = length + 7;
    if (!output || capacity < frameLength || length > kMaxPayload
        || (length && !payload)) return 0;
    output[0] = makcu::FRAME_MAGIC0;
    output[1] = makcu::FRAME_MAGIC1;
    output[2] = static_cast<uint8_t>(length);
    output[3] = ++sequence_;
    output[4] = command;
    if (length) std::copy(payload, payload + length, output + 5);
    const uint16_t crc = makcu::crc16_modbus(output + 2, length + 3);
    output[frameLength - 2] = static_cast<uint8_t>(crc & 0xFF);
    output[frameLength - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    return frameLength;
}

bool MakcuNewConnection::sendFrame(
    uint8_t command, const uint8_t* payload, size_t length)
{
    if (!portOpen_.load(std::memory_order_acquire)
        || length > kMaxPayload || (length && !payload)) return false;

    uint8_t frame[kMaxPayload + 7];
    std::lock_guard<std::mutex> lock(writeMutex_);
    const size_t frameLength = encodeFrameLocked(
        frame, sizeof(frame), command, payload, length);
    if (!frameLength) return false;
    try
    {
        const size_t written = serial_.write(frame, frameLength);
        return written == frameLength;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MakcuNew] serial write failed: " << e.what() << std::endl;
        portOpen_.store(false);
        open_.store(false);
        return false;
    }
    catch (...)
    {
        std::cerr << "[MakcuNew] serial write failed: unknown exception." << std::endl;
        portOpen_.store(false);
        open_.store(false);
        return false;
    }
}

void MakcuNewConnection::sendDeadBaud(unsigned int baud)
{
    uint8_t frame[9];
    frame[0] = 0xDE;
    frame[1] = 0xAD;
    frame[2] = 0x05;
    frame[3] = 0x00;
    frame[4] = 0xA5;
    frame[5] = static_cast<uint8_t>(baud & 0xFF);
    frame[6] = static_cast<uint8_t>((baud >> 8) & 0xFF);
    frame[7] = static_cast<uint8_t>((baud >> 16) & 0xFF);
    frame[8] = static_cast<uint8_t>((baud >> 24) & 0xFF);

    std::lock_guard<std::mutex> lock(writeMutex_);
    if (!portOpen_.load(std::memory_order_acquire)) return;
    try { serial_.write(frame, sizeof(frame)); }
    catch (...) { portOpen_.store(false);
 open_.store(false); }
}

void MakcuNewConnection::subscribeAsync(bool on)
{
    const uint8_t enable = on ? 1 : 0;
    sendFrame(makcu::CMD_SUB_ASYNC, &enable, 1);
}

void MakcuNewConnection::feedAsciiByte(uint8_t b)
{
    if (b == '\n')
    {
        std::lock_guard<std::mutex> lk(asciiMutex_);
        if (!asciiAccum_.empty())
        {
            asciiLine_ = asciiAccum_;
            asciiAccum_.clear();
            ++asciiSeq_;
            asciiCv_.notify_all();
        }
        return;
    }

    if (b == '\r') return;

    if (b >= 0x20 && b < 0x7F)
    {
        std::lock_guard<std::mutex> lk(asciiMutex_);
        if (asciiAccum_.size() < 256) asciiAccum_.push_back(static_cast<char>(b));
        return;
    }

    if (b < 0x20)
    {
        applyPhysicalButtons(b);
        return;
    }

    std::lock_guard<std::mutex> lk(asciiMutex_);
    asciiAccum_.clear();
}

bool MakcuNewConnection::probeAscii(const std::string& command,
                                    const std::string& expect,
                                    unsigned int timeoutMs)
{
    std::unique_lock<std::mutex> lk(asciiMutex_);
    asciiLine_.clear();
    asciiAccum_.clear();
    const uint64_t baseSeq = asciiSeq_;
    lk.unlock();

    std::string wire = command;
    wire += "\r\n";
    {
        std::lock_guard<std::mutex> wl(writeMutex_);
        if (!portOpen_.load(std::memory_order_acquire)) return false;
        try
        {
            if (serial_.write(reinterpret_cast<const uint8_t*>(wire.data()), wire.size())
                != wire.size())
                return false;
        }
        catch (...)
        {
            return false;
        }
    }

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);

    std::unique_lock<std::mutex> lock(asciiMutex_);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (asciiSeq_ != baseSeq
            && asciiLine_.find(expect) != std::string::npos)
        {
            std::cout << "[MakcuNew] probe reply: " << asciiLine_ << std::endl;
            return true;
        }
        asciiCv_.wait_for(lock, std::chrono::milliseconds(20));
    }
    return false;
}

bool MakcuNewConnection::initializeProtocolSession()
{
    realButtons_.store(0, std::memory_order_release);
    injectedButtons_.store(0, std::memory_order_release);

    subscribeAsync(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return true;
}

uint8_t MakcuNewConnection::buttonBit(int button)
{
    return button >= 1 && button <= 5
        ? static_cast<uint8_t>(1u << (button - 1)) : 0;
}

bool MakcuNewConnection::move(int x, int y)
{
    if (!open_.load(std::memory_order_acquire)) return false;
    const int cx = std::clamp(x, -32768, 32767);
    const int cy = std::clamp(y, -32768, 32767);
    if (cx == 0 && cy == 0) return true;

    uint8_t payload[4];
    payload[0] = static_cast<uint8_t>(cx & 0xFF);
    payload[1] = static_cast<uint8_t>((cx >> 8) & 0xFF);
    payload[2] = static_cast<uint8_t>(cy & 0xFF);
    payload[3] = static_cast<uint8_t>((cy >> 8) & 0xFF);
    return sendFrame(makcu::CMD_MOVE, payload, sizeof(payload));
}

void MakcuNewConnection::cancelMove()
{
    if (!open_.load(std::memory_order_acquire)) return;
    sendFrame(makcu::CMD_MOVE_CANCEL, nullptr, 0);
}

void MakcuNewConnection::press(int button)
{
    if (!open_.load(std::memory_order_acquire)) return;
    const uint8_t bit = buttonBit(button);
    if (!bit) return;
    const uint8_t state = static_cast<uint8_t>(outputButtons_.fetch_or(bit) | bit);
    sendFrame(makcu::CMD_BUTTON_MASK, &state, 1);
}

void MakcuNewConnection::release(int button)
{
    if (!open_.load(std::memory_order_acquire)) return;
    const uint8_t bit = buttonBit(button);
    if (!bit) return;
    const uint8_t inverse = static_cast<uint8_t>(~bit);
    const uint8_t state = static_cast<uint8_t>(outputButtons_.fetch_and(inverse) & inverse);
    sendFrame(makcu::CMD_BUTTON_MASK, &state, 1);
}

void MakcuNewConnection::click(int button)
{
    if (!open_.load(std::memory_order_acquire)) return;
    const uint8_t bit = buttonBit(button);
    if (!bit) return;
    uint8_t payload[3];
    payload[0] = bit;
    payload[1] = 45;
    payload[2] = 0;
    sendFrame(makcu::CMD_CLICK, payload, sizeof(payload));
}

void MakcuNewConnection::wheel(int delta)
{
    if (!open_.load(std::memory_order_acquire)) return;
    const int8_t value = static_cast<int8_t>(std::clamp(delta, -127, 127));
    sendFrame(makcu::CMD_WHEEL, reinterpret_cast<const uint8_t*>(&value), 1);
}

bool MakcuNewConnection::tapKey(int hidKey, int holdMs, int mod)
{
    if (!open_.load(std::memory_order_acquire)) return false;
    if (hidKey <= 0 || hidKey > 0xFF) return false;
    const int hold = std::clamp(holdMs, 1, 2000);
    uint8_t payload[4];
    payload[0] = static_cast<uint8_t>(std::clamp(mod, 0, 0xFF));
    payload[1] = static_cast<uint8_t>(hidKey);
    payload[2] = static_cast<uint8_t>(hold & 0xFF);
    payload[3] = static_cast<uint8_t>((hold >> 8) & 0xFF);
    return sendFrame(makcu::CMD_KEY_TAP, payload, sizeof(payload));
}

bool MakcuNewConnection::physicalButtonPressed(int button) const
{
    const uint8_t bit = buttonBit(button);
    return bit && (realButtons_.load(std::memory_order_acquire) & bit) != 0;
}

void MakcuNewConnection::readerLoop()
{
    uint8_t chunk[512];
    while (!stopping_.load())
    {
        try
        {
            const size_t readable = serial_.available();
            if (readable == 0)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            const size_t count = serial_.read(
                chunk, std::min(readable, sizeof(chunk)));
            if (count)
            {
                receiveBuffer_.insert(receiveBuffer_.end(), chunk, chunk + count);
                consumeFrames();
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[MakcuNew] serial reader failed: " << e.what() << std::endl;
            portOpen_.store(false);
            open_.store(false);
            break;
        }
        catch (...)
        {
            std::cerr << "[MakcuNew] serial reader failed: unknown exception." << std::endl;
            portOpen_.store(false);
            open_.store(false);
            break;
        }
    }
}

void MakcuNewConnection::consumeFrames()
{
    for (;;)
    {
        const size_t avail = receiveBuffer_.size() - receiveOffset_;
        if (avail == 0) break;

        const uint8_t* p = receiveBuffer_.data() + receiveOffset_;

        if (p[0] != makcu::FRAME_MAGIC0)
        {
            feedAsciiByte(p[0]);
            ++receiveOffset_;
            continue;
        }

        if (avail < 2) break;

        if (p[1] != makcu::FRAME_MAGIC1)
        {
            feedAsciiByte(p[0]);
            ++receiveOffset_;
            continue;
        }

        if (avail < 7) break;
        const size_t payloadLength = p[2];
        if (payloadLength > kMaxPayload)
        {
            feedAsciiByte(p[0]);
            ++receiveOffset_;
            continue;
        }
        const size_t frameLength = payloadLength + 7;
        if (avail < frameLength) break;

        const uint16_t receivedCrc = static_cast<uint16_t>(
            p[frameLength - 2] | (p[frameLength - 1] << 8));
        const uint16_t expectedCrc = makcu::crc16_modbus(p + 2, payloadLength + 3);

        if (receivedCrc == expectedCrc)
        {
            const uint8_t command = p[4];
            const uint8_t* payload = p + 5;

            if (command == 0x84)
            {
                if (payloadLength >= 1)
                {
                    applyPhysicalButtons(payload[0]);
                    if (payloadLength >= 2)
                        injectedButtons_.store(payload[1],
                                               std::memory_order_release);
                }
            }
        }
        receiveOffset_ += frameLength;
    }

    if (receiveOffset_
        && (receiveOffset_ >= 4096 || receiveOffset_ * 2 >= receiveBuffer_.size()))
    {
        receiveBuffer_.erase(
            receiveBuffer_.begin(), receiveBuffer_.begin() + receiveOffset_);
        receiveOffset_ = 0;
    }
}

// ---- 瞬时屏蔽真实输入 ----
//
// 走 ASCII 文本通道(与 km.version 探活同一条路), 不新增二进制帧:
//   固件 fw_device 收到 "km.mask(N)" 后经板间 UART 转发给 fw_host,
//   由 fw_host 在其 mask 窗口内丢弃真实键鼠输入。
//
// 时序注意: 屏蔽是"从固件收到那一刻起算 N 毫秒", 本函数不等任何应答 ——
// 固件对这条命令只回一行 "km.mask(N) ok" 到板间链路, 不会回到本串口
// (设备侧的 ASCII 应答仅对 km.version 等命令发出), 因此这里写完即认为成功,
// 与协议"上位机必须 ACK-free"的整体约定一致。
bool MakcuNewConnection::mask(int durationMs)
{
    if (!open_.load(std::memory_order_acquire)) return false;

    // 钳制到固件侧的上限(见 fw_host EspUsbHost.h kMaskMaxMs = 2000)。
    // 超上限固件会自行截到 2000, 这里先钳一次以便行为可预期。
    if (durationMs < 0) durationMs = 0;
    if (durationMs > 2000) durationMs = 2000;

    // <=0 走解除命令, 与固件的 km.maskoff 语义一致。
    if (durationMs == 0) return maskOff();

    char buf[32];
    std::snprintf(buf, sizeof(buf), "km.mask(%d)\r\n", durationMs);

    std::lock_guard<std::mutex> lock(writeMutex_);
    if (!portOpen_.load(std::memory_order_acquire)) return false;
    try
    {
        const size_t n = std::strlen(buf);
        return serial_.write(reinterpret_cast<const uint8_t*>(buf), n) == n;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MakcuNew] mask write failed: " << e.what() << std::endl;
        portOpen_.store(false);
        open_.store(false);
        return false;
    }
    catch (...)
    {
        std::cerr << "[MakcuNew] mask write failed: unknown exception." << std::endl;
        portOpen_.store(false);
        open_.store(false);
        return false;
    }
}

bool MakcuNewConnection::maskOff()
{
    if (!open_.load(std::memory_order_acquire)) return false;

    static const char kOff[] = "km.maskoff\r\n";
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (!portOpen_.load(std::memory_order_acquire)) return false;
    try
    {
        const size_t n = sizeof(kOff) - 1;
        return serial_.write(reinterpret_cast<const uint8_t*>(kOff), n) == n;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MakcuNew] maskoff write failed: " << e.what() << std::endl;
        portOpen_.store(false);
        open_.store(false);
        return false;
    }
    catch (...)
    {
        std::cerr << "[MakcuNew] maskoff write failed: unknown exception." << std::endl;
        portOpen_.store(false);
        open_.store(false);
        return false;
    }
}

void MakcuNewConnection::applyPhysicalButtons(uint8_t mask)
{
    realButtons_.store(static_cast<uint8_t>(mask & 0x1F),
                       std::memory_order_release);
}
