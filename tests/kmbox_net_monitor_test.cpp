#include "mouse/kmbox_net/kmboxNet.h"
#include "keyboard/hotkey_selection.h"
#include "macro/macro_config.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main()
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa)) return 1;
    SOCKET server = socket(AF_INET, SOCK_DGRAM, 0);
    SOCKET occupied = socket(AF_INET, SOCK_DGRAM, 0);
    auto bindLocal = [](SOCKET socket) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return address;
        int size = sizeof(address);
        getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size);
        return address;
    };
    const auto endpoint = bindLocal(server);
    const auto monitor = bindLocal(occupied);
    if (!endpoint.sin_port || !monitor.sin_port) return 2;
    DWORD timeout = 100;
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    std::atomic<bool> done{false};
    std::thread responder([&] {
        while (!done.load()) {
            cmd_head_t request{};
            sockaddr_in peer{};
            int size = sizeof(peer);
            const int count = recvfrom(server, reinterpret_cast<char*>(&request), sizeof(request), 0,
                                       reinterpret_cast<sockaddr*>(&peer), &size);
            if (count == sizeof(request))
                sendto(server, reinterpret_cast<const char*>(&request), sizeof(request), 0,
                       reinterpret_cast<sockaddr*>(&peer), size);
        }
    });
    int failures = 0;
    auto check = [&](bool okay, const char* message) {
        if (!okay) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    auto await = [](auto predicate) {
        for (int i = 0; i < 100; ++i) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    const std::string port = std::to_string(ntohs(endpoint.sin_port));
    check(kmNet_init((char*)"127.0.0.1", const_cast<char*>(port.c_str()), (char*)"1234ABCD") == 0, "connect");
    const short monitorPort = static_cast<short>(ntohs(monitor.sin_port));
    check(kmNet_monitor(monitorPort) == err_net_monitor_bind, "occupied monitor port reports failure");
    check(kmNet_monitor_keyboard(30) == -1, "failed monitor cannot pretend keys are up");
    closesocket(occupied);
    check(kmNet_monitor(monitorPort) == 0, "monitor starts on a free port");
    check(kmNet_monitor_keyboard(30) == -1, "monitor awaits first complete hardware report");
    std::array<unsigned char,20> report{}; // Existing protocol: 8 mouse bytes + 12 keyboard bytes.
    auto send = [&](int count = 20) {
        sendto(server, reinterpret_cast<const char*>(report.data()), count, 0,
               reinterpret_cast<const sockaddr*>(&monitor), sizeof(monitor));
    };
    send();
    check(await([&] { return kmNet_monitor_keyboard(30) == 0; }), "released report received");
    std::vector<HotkeyProfile> profiles(2);
    for (auto& profile : profiles) { profile.group = "net"; profile.keys = {"RightMouseButton"}; }
    profiles[0].activation_key = "Key1";
    profiles[1].activation_key = "Key2";
    profiles[0].activation_selected = true;
    HotkeyActivationSampler sampler;
    auto sample = [&] { return sampler.sample(profiles, "net", [&](const auto& key) {
        return kmNet_monitor_keyboard(static_cast<short>(macros::hidKey(key))) > 0;
    }); };
    sample();
    report[10] = 31; // USB HID keyboard 2.
    send();
    check(await([&] { return kmNet_monitor_keyboard(31) == 1; }), "NET keyboard 2 received");
    check(sample() && preferredHotkey(profiles,0) == 1, "NET key activates entire second profile before aiming");
    report[1] = 2;
    report[9] = 2; // Left shift.
    send();
    check(await([&] { return kmNet_monitor_mouse_right() == 1 && kmNet_monitor_keyboard(225) == 1; }), "mouse and modifier received together");
    check(selectActiveHotkeyIndex(profiles,"net",[](const auto&) { return kmNet_monitor_mouse_right() > 0; }) == 1,
          "right mouse resolves to NET-selected profile");
    report[10] = 30;
    send(10); // No keyboard key array: must not copy uninitialized bytes.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(kmNet_monitor_keyboard(31) == 1 && kmNet_monitor_keyboard(30) == 0, "short report does not corrupt keys");
    report.fill(0); send();
    check(await([&] { return kmNet_monitor_keyboard(31) == 0; }), "release received");
    sample();
    report[10] = 30; send();
    check(await([&] { return kmNet_monitor_keyboard(30) == 1; }), "NET keyboard 1 received");
    check(sample() && preferredHotkey(profiles,0) == 0, "NET keyboard 1 returns to first whole profile");
    check(kmNet_monitor(monitorPort) == 0, "monitor restarts without competing receiver threads");
    check(kmNet_monitor_keyboard(30) == -1, "restart clears stale held keys");
    report.fill(0); send();
    check(await([&] { return kmNet_monitor_keyboard(30) == 0; }), "restarted monitor receives reports");
    check(kmNet_monitor(0) == 0 && kmNet_monitor_mouse_right() == -1, "stopped monitor releases its state");
    kmNet_close();
    done = true;
    responder.join();
    closesocket(server);
    WSACleanup();
    return failures ? 3 : 0;
}
