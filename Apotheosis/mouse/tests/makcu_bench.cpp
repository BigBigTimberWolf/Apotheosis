#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../MakcuNew.h"
#include "../../modules/makcu/include/makcu.h"

namespace
{

using SteadyClock = std::chrono::steady_clock;

const char* C(const char8_t* s) { return reinterpret_cast<const char*>(s); }

double us_between(SteadyClock::time_point a, SteadyClock::time_point b)
{
    return std::chrono::duration<double, std::micro>(b - a).count();
}

struct Stats
{
    size_t count = 0;
    double mean = 0.0, p50 = 0.0, p95 = 0.0, p99 = 0.0, max = 0.0;
};

Stats summarize(std::vector<double> v)
{
    Stats s;
    if (v.empty()) return s;
    s.count = v.size();
    std::sort(v.begin(), v.end());
    double sum = 0.0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    s.p50 = v[static_cast<size_t>(0.50 * static_cast<double>(v.size() - 1))];
    s.p95 = v[static_cast<size_t>(0.95 * static_cast<double>(v.size() - 1))];
    s.p99 = v[static_cast<size_t>(0.99 * static_cast<double>(v.size() - 1))];
    s.max = v.back();
    return s;
}

struct Backend
{
    virtual ~Backend() = default;
    virtual const char* label() const = 0;
    virtual unsigned baud() const = 0;
    virtual bool open(std::string& err) = 0;
    virtual void close() = 0;
    virtual bool move(int dx, int dy) = 0;
};

struct MakcuNewBackend final : Backend
{
    std::string port;
    unsigned baudRate;
    std::unique_ptr<MakcuNewConnection> conn;

    MakcuNewBackend(std::string p, unsigned b) : port(std::move(p)), baudRate(b) {}

    const char* label() const override { return "MAKCUNEW"; }
    unsigned baud() const override { return baudRate; }

    bool open(std::string& err) override
    {
        conn = std::make_unique<MakcuNewConnection>(port, baudRate);
        if (!conn->isOpen())
        {
            err = "会话握手失败(端口被占用 / 设备未连接 / 波特率切换失败)";
            conn.reset();
            return false;
        }
        return true;
    }

    void close() override { conn.reset(); }
    bool move(int dx, int dy) override { return conn && conn->move(dx, dy); }
};

struct MakcuBackend final : Backend
{
    std::string port;
    unsigned baudRate;
    std::unique_ptr<makcu::Device> dev;

    MakcuBackend(std::string p, unsigned b) : port(std::move(p)), baudRate(b) {}

    const char* label() const override { return "MAKCU"; }
    unsigned baud() const override { return baudRate; }

    bool open(std::string& err) override
    {
        dev = std::make_unique<makcu::Device>();
        if (!dev->connect(port))
        {
            err = "连接失败(端口被占用 / 设备未连接)";
            dev.reset();
            return false;
        }
        if (baudRate != 115200 && !dev->setBaudRate(baudRate, true))
        {
            err = "设置波特率失败";
            dev->disconnect();
            dev.reset();
            return false;
        }
        return true;
    }

    void close() override
    {
        if (dev) { dev->disconnect(); dev.reset(); }
    }

    bool move(int dx, int dy) override { return dev && dev->mouseMove(dx, dy); }
};

struct Options
{
    std::string makcuPort = "COM5";
    unsigned makcuBaud = 115200;
    std::string newPort = "COM3";
    unsigned newBaud = 6000000;
    int count = 4000;
    int warmup = 200;
    double satSeconds = 1.5;
    double tickSeconds = 2.0;
    bool force = false;
    std::string only = "both";
};

struct Result
{
    std::string label;
    unsigned baudRate = 0;
    bool ok = false;
    std::string err;
    double openMs = 0.0;
    size_t fails = 0;
    Stats single;
    double satRate = 0.0;
    Stats satCall;
    Stats tickGap;
    Stats tickCost;
    size_t late = 0;
};

constexpr double kTickPeriodUs = 1000000.0 / 120.0;

void runBackend(Backend& b, const Options& o, Result& r)
{
    r.label = b.label();
    r.baudRate = b.baud();

    std::string err;
    const auto tOpen0 = SteadyClock::now();
    const bool opened = b.open(err);
    r.openMs = us_between(tOpen0, SteadyClock::now()) / 1000.0;
    if (!opened) { r.err = err; return; }
    r.ok = true;

    int sign = 1;

    for (int i = 0; i < o.warmup; ++i) { sign = -sign; b.move(sign, 0); }

    {
        std::vector<double> calls;
        calls.reserve(static_cast<size_t>(o.count));
        for (int i = 0; i < o.count; ++i)
        {
            sign = -sign;
            const auto t0 = SteadyClock::now();
            const bool ok = b.move(sign, 0);
            const auto t1 = SteadyClock::now();
            if (!ok) ++r.fails;
            calls.push_back(us_between(t0, t1));
        }
        r.single = summarize(std::move(calls));
    }

    {
        std::vector<double> calls;
        sign = 1;
        const auto t0 = SteadyClock::now();
        const auto deadline = t0 + std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(o.satSeconds));
        while (SteadyClock::now() < deadline)
        {
            sign = -sign;
            const auto c0 = SteadyClock::now();
            const bool ok = b.move(sign, 0);
            const auto c1 = SteadyClock::now();
            if (!ok) ++r.fails;
            calls.push_back(us_between(c0, c1));
        }
        const double secs = us_between(t0, SteadyClock::now()) / 1e6;
        r.satRate = secs > 0.0 ? static_cast<double>(calls.size()) / secs : 0.0;
        r.satCall = summarize(std::move(calls));
    }

    {
        const int ticks = static_cast<int>(o.tickSeconds * 120.0);
        std::vector<double> gaps, costs;
        gaps.reserve(static_cast<size_t>(ticks));
        costs.reserve(static_cast<size_t>(ticks));

        auto next = SteadyClock::now();
        auto prevStart = next;
        for (int i = 0; i < ticks; ++i)
        {
            next += std::chrono::nanoseconds(8333333);
            std::this_thread::sleep_until(next - std::chrono::microseconds(400));
            while (SteadyClock::now() < next) {   }

            const auto c0 = SteadyClock::now();
            sign = -sign;
            const bool ok = b.move(sign, 0);
            const auto c1 = SteadyClock::now();
            if (!ok) ++r.fails;

            if (i > 0)
            {
                const double gap = us_between(prevStart, c0);
                gaps.push_back(gap);
                if (gap > kTickPeriodUs + 2000.0) ++r.late;
            }
            prevStart = c0;
            costs.push_back(us_between(c0, c1));
        }
        r.tickGap = summarize(std::move(gaps));
        r.tickCost = summarize(std::move(costs));
    }

    b.close();
}

bool apotheosisRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"Apotheosis.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool parsePortSpec(const std::string& spec, std::string& port, unsigned& baud)
{
    const size_t colon = spec.find(':');
    if (colon == std::string::npos) { port = spec; return true; }
    port = spec.substr(0, colon);
    const int v = std::atoi(spec.c_str() + colon + 1);
    if (v <= 0) return false;
    baud = static_cast<unsigned>(v);
    return true;
}

void printStats(const char* what, const Stats& s)
{
    std::cout << "    " << std::left << std::setw(22) << what
              << " n=" << std::setw(6) << s.count
              << " mean=" << std::setw(9) << std::fixed << std::setprecision(2) << s.mean
              << " p50=" << std::setw(9) << s.p50
              << " p95=" << std::setw(9) << s.p95
              << " p99=" << std::setw(9) << s.p99
              << " max=" << std::setw(10) << s.max << " (us)\n";
}

}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
        if (a == "--makcu") { std::string s; next(s); parsePortSpec(s, o.makcuPort, o.makcuBaud); }
        else if (a == "--new") { std::string s; next(s); parsePortSpec(s, o.newPort, o.newBaud); }
        else if (a == "--count") { std::string s; next(s); o.count = std::max(100, std::atoi(s.c_str())); }
        else if (a == "--sat") { std::string s; next(s); o.satSeconds = std::atof(s.c_str()); }
        else if (a == "--ticks") { std::string s; next(s); o.tickSeconds = std::atof(s.c_str()); }
        else if (a == "--only") { next(o.only); }
        else if (a == "--force") { o.force = true; }
        else if (a == "--help" || a == "-h")
        {
            std::cout << C(u8"用法: makcu_bench.exe [--makcu COM5:115200] [--new COM3:6000000]")
                      << "\n"
                      << C(u8"                      [--count 4000] [--sat 1.5] [--ticks 2.0]")
                      << "\n"
                      << C(u8"                      [--only both|makcu|new] [--force]") << "\n";
            return 0;
        }
        else
        {
            std::cout << C(u8"未知参数: ") << a << "\n";
            return 2;
        }
    }

    std::cout << C(u8"=== MAKCU vs MAKCUNEW 链路性能对比 ===") << "\n";
    std::cout << C(u8"只发 ±1px 交替位移(净位移 0), 不点击/不拖动; 串口独占, 需先关闭 Apotheosis。") << "\n";
    std::cout << "MAKCU    = COM5@115200 ASCII \"km.move(x,y)\\r\\n\" (14~15 字节, sendCommand 内含 FlushFileBuffers)\n";
    std::cout << "MAKCUNEW = COM3@6000000 二进制 11 字节定长帧 (无 ACK, 写完即返回)\n";

    if (!o.force && apotheosisRunning())
    {
        std::cout << C(u8"\n[拒绝执行] 检测到 Apotheosis 正在运行: 串口已被占用, 且测出来的数会被主程序干扰。")
                  << "\n"
                  << C(u8"           请先关闭主程序; 确实要硬跑请加 --force。") << "\n";
        return 3;
    }

    std::vector<std::unique_ptr<Backend>> backends;
    if (o.only == "both" || o.only == "makcu")
        backends.push_back(std::make_unique<MakcuBackend>(o.makcuPort, o.makcuBaud));
    if (o.only == "both" || o.only == "new")
        backends.push_back(std::make_unique<MakcuNewBackend>(o.newPort, o.newBaud));

    std::vector<Result> results;
    results.reserve(backends.size());
    for (auto& b : backends)
    {
        Result r;
        std::cout << "\n---- " << b->label() << " ----\n";
        runBackend(*b, o, r);
        if (!r.ok)
        {
            std::cout << "  " << C(u8"不可用: ") << r.err << "\n";
        }
        else
        {
            std::cout << "  " << C(u8"打开/握手耗时 ") << std::fixed << std::setprecision(1)
                      << r.openMs << " ms @ " << r.baudRate << " baud\n";
            printStats(C(u8"单发指令耗时"), r.single);
            std::cout << "    " << std::left << std::setw(22) << C(u8"饱和吞吐")
                      << " " << std::fixed << std::setprecision(0) << r.satRate
                      << C(u8" 条/秒  (饱和下单发 mean ")
                      << std::setprecision(2) << r.satCall.mean << " us)\n";
            printStats(C(u8"120Hz 实际间隔"), r.tickGap);
            std::cout << "    " << std::left << std::setw(22) << C(u8"120Hz 迟到次数")
                      << " " << r.late << C(u8" / ") << r.tickGap.count
                      << C(u8"  (>+2ms)   发指令耗时 mean ")
                      << std::setprecision(2) << r.tickCost.mean
                      << " us  max " << r.tickCost.max << " us\n";
            std::cout << "    " << std::left << std::setw(22) << C(u8"发送失败")
                      << " " << r.fails << "\n";
        }
        results.push_back(std::move(r));
    }

    if (results.size() == 2 && results[0].ok && results[1].ok)
    {
        const Result& a = results[0];
        const Result& b = results[1];
        std::cout << "\n" << C(u8"=== 对比 ===") << "\n";
        const double ratioCall = a.single.mean > 0.0 ? b.single.mean / a.single.mean : 0.0;
        const double ratioRate = a.satRate > 0.0 ? b.satRate / a.satRate : 0.0;
        std::cout << std::fixed << std::setprecision(2)
                  << "  " << C(u8"单发耗时 ") << a.label << " " << a.single.mean << " us  vs  "
                  << b.label << " " << b.single.mean << " us   ->  "
                  << (ratioCall < 1.0 ? C(u8"后者更快 ") : C(u8"后者更慢 "))
                  << (ratioCall < 1.0 ? 1.0 / (ratioCall > 0 ? ratioCall : 1.0) : ratioCall) << "x\n"
                  << "  " << C(u8"饱和吞吐 ") << a.label << " " << std::setprecision(0) << a.satRate
                  << " 条/秒  vs  " << b.label << " " << b.satRate
                  << C(u8" 条/秒   ->  倍率 ") << std::setprecision(2) << ratioRate << "x\n"
                  << "  " << C(u8"120Hz 迟到 ") << a.label << " " << a.late << " 次  vs  "
                  << b.label << " " << b.late << C(u8" 次\n");
    }

    std::cout << "\n" << C(u8"提示: 主程序里 MAKCUNEW 是在瞄准线程内联发送(move 的耗时=瞄准线程停顿),")
              << "\n"
              << C(u8"      MAKCU 走 latest-only 队列 + 工作线程(瞄准线程只付入队代价)。") << "\n";
    return 0;
}
