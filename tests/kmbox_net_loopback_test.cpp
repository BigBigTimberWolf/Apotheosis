#include "mouse/kmbox_net/kmboxNet.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

int main()
{
    if (kmNet_init((char*)"127.0.0.1", (char*)"6234", (char*)"12345")
        != err_net_invalid_config)
        return 1;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) return 2;
    SOCKET server = socket(AF_INET, SOCK_DGRAM, 0);
    if (server == INVALID_SOCKET) return 3;
    DWORD timeout_ms = 1500;
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) return 4;
    int local_len = sizeof(local);
    if (getsockname(server, reinterpret_cast<sockaddr*>(&local), &local_len) != 0) return 5;
    const std::string port = std::to_string(ntohs(local.sin_port));

    std::atomic<int> packets{0};
    std::atomic<bool> done{false};
    std::thread responder([&] {
        while (!done.load())
        {
            cmd_head_t request{};
            sockaddr_in peer{};
            int peer_len = sizeof(peer);
            const int got = recvfrom(server, reinterpret_cast<char*>(&request), sizeof(request),
                                     0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (got != sizeof(request)) continue;
            if (request.cmd != cmd_connect || request.mac != 0x1234ABCD) continue;
            const int seen = ++packets;
            if (seen == 1) std::this_thread::sleep_for(std::chrono::milliseconds(750));
            sendto(server, reinterpret_cast<const char*>(&request), sizeof(request), 0,
                   reinterpret_cast<sockaddr*>(&peer), peer_len);
        }
    });

    const int first = kmNet_init((char*)"127.0.0.1", const_cast<char*>(port.c_str()),
                                 (char*)"1234ABCD");
    kmNet_close();
    const int second = kmNet_init((char*)"127.0.0.1", const_cast<char*>(port.c_str()),
                                  (char*)"1234ABCD");
    kmNet_close();

    done = true;
    closesocket(server);
    responder.join();
    WSACleanup();
    if (first != 0 || second != 0 || packets.load() < 2)
    {
        std::printf("KMBoxNet handshake failed: first=%d second=%d packets=%d\n",
                    first, second, packets.load());
        return 6;
    }
    return 0;
}
