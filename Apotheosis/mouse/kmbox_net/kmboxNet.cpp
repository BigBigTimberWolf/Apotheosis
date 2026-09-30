#include <time.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>

#include "kmbox_net/kmboxNet.h"
#include "kmbox_net/HidTable.h"

#define monitor_ok    2
#define monitor_exit  0
SOCKET sockClientfd = 0;
SOCKET sockMonitorfd = 0;
client_tx tx;
client_tx rx;
SOCKADDR_IN addrSrv;
soft_mouse_t    softmouse;
soft_keyboard_t softkeyboard;
static std::atomic<int> monitor_run{0};
static HANDLE monitor_thread_handle = nullptr;
static std::mutex monitor_state_mutex;
static bool monitor_has_report = false;
static void stopMonitor();
static int mask_keyboard_mouse_flag = 0;
static bool client_winsock_started = false;
static int last_socket_error = 0;

#pragma pack(1)
typedef struct {
	unsigned char report_id;
	unsigned char buttons;
	short x;
	short y;
	short wheel;
}standard_mouse_report_t;

typedef struct {
	unsigned char report_id;
	unsigned char buttons;
	unsigned char data[10];
} standard_keyboard_report_t;
#pragma pack()

standard_mouse_report_t     hw_mouse;
standard_keyboard_report_t  hw_keyboard;

int myrand(int a, int b)
{
	int min = a < b ? a : b;
	int max = a > b ? a : b;
	return ((rand() % (max - min)) + min);
}

unsigned int StrToHex(char* pbSrc, int nLen)
{
	char h1, h2;
	unsigned char s1, s2;
	int i;
	unsigned int pbDest[16] = { 0 };
	for (i = 0; i < nLen; i++) {
		h1 = pbSrc[2 * i];
		h2 = pbSrc[2 * i + 1];
		s1 = toupper(h1) - 0x30;
		if (s1 > 9)
			s1 -= 7;
		s2 = toupper(h2) - 0x30;
		if (s2 > 9)
			s2 -= 7;
		pbDest[i] = s1 * 16 + s2;
	}
	return pbDest[0] << 24 | pbDest[1] << 16 | pbDest[2] << 8 | pbDest[3];
}

int NetRxReturnHandle(client_tx* rx, client_tx* tx)
{
	if (rx->head.cmd != tx->head.cmd)
		return  err_net_cmd;
	if (rx->head.indexpts != tx->head.indexpts)
		return  err_net_pts;
	return 0;
}

int kmNet_init(char* ip, char* port, char* mac)
{
	last_socket_error = 0;
	// The packet header contains four UUID bytes. The old parser read eight
	// characters unconditionally, including beyond the string for a short value.
	if (!ip || !port || !mac || std::strlen(mac) != 8)
		return err_net_invalid_config;
	for (int i = 0; i < 8; ++i)
		if (!std::isxdigit(static_cast<unsigned char>(mac[i])))
			return err_net_invalid_config;
	char* port_end = nullptr;
	const long parsed_port = std::strtol(port, &port_end, 10);
	if (port_end == port || *port_end != '\0' || parsed_port <= 0 || parsed_port > 65535)
		return err_net_invalid_config;

	WSADATA wsaData{};
	int err = WSAStartup(MAKEWORD(1, 1), &wsaData);
	if (err != 0) { last_socket_error = err; return err_creat_socket; }
	client_winsock_started = true;
	if (inet_addr(ip) == INADDR_NONE) {
		kmNet_close();
		return err_net_invalid_config;
	}
	if (LOBYTE(wsaData.wVersion) != 1 || HIBYTE(wsaData.wVersion) != 1) {
		kmNet_close();
		return err_net_version;
	}
	srand((unsigned)time(NULL));
	sockClientfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockClientfd == INVALID_SOCKET) {
		last_socket_error = WSAGetLastError();
		kmNet_close();
		return err_creat_socket;
	}
	// The official client allows a longer handshake. 200 ms per attempt can
	// exhaust all retries before a slow device or LAN path returns its first reply.
	DWORD timeout_ms = 1000;
	setsockopt(sockClientfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
	addrSrv.sin_addr.S_un.S_addr = inet_addr(ip);
	addrSrv.sin_family = AF_INET;
	addrSrv.sin_port = htons(static_cast<unsigned short>(parsed_port));
	tx.head.mac = StrToHex(mac, 4);
	tx.head.rand = rand();
	tx.head.indexpts = 0;
	tx.head.cmd = cmd_connect;
	memset(&softmouse, 0, sizeof(softmouse));
	memset(&softkeyboard, 0, sizeof(softkeyboard));
	int result = err_net_rx_timeout;
	last_socket_error = 0;
	for (int attempt = 0; attempt < 3; ++attempt) {
		err = sendto(sockClientfd, (const char*)&tx, sizeof(cmd_head_t), 0,
			(struct sockaddr*)&addrSrv, sizeof(addrSrv));
		if (err == SOCKET_ERROR) {
			last_socket_error = WSAGetLastError();
			result = err_net_tx;
			continue;
		}
		if (attempt == 0) Sleep(20);
		SOCKADDR_IN responder{};
		int clen = sizeof(responder);
		err = recvfrom(sockClientfd, (char*)&rx, sizeof(rx), 0,
			(struct sockaddr*)&responder, &clen);
		if (err == SOCKET_ERROR) {
			last_socket_error = WSAGetLastError();
			result = err_net_rx_timeout;
			continue;
		}
		if (err < static_cast<int>(sizeof(cmd_head_t))) {
			result = err_net_cmd;
			continue;
		}
		result = NetRxReturnHandle(&rx, &tx);
		if (result == 0) return 0;
	}
	kmNet_close();
	return result;
}

void kmNet_close()
{
	stopMonitor();
	if (sockClientfd != 0 && sockClientfd != INVALID_SOCKET)
		closesocket(sockClientfd);
	sockClientfd = 0;
	if (client_winsock_started) {
		WSACleanup();
		client_winsock_started = false;
	}
}

int kmNet_last_socket_error()
{
	return last_socket_error;
}

int kmNet_mouse_move(short x, short y)
{
	int err;
	if (sockClientfd <= 0)       return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mouse_move;
	tx.head.rand = rand();
	softmouse.x = x;
	softmouse.y = y;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	softmouse.x = 0;
	softmouse.y = 0;
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mouse_left(int isdown)
{
	int err;
	if (sockClientfd <= 0)       return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mouse_left;
	tx.head.rand = rand();
	softmouse.button = (isdown ? (softmouse.button | 0x01) : (softmouse.button & (~0x01)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mouse_middle(int isdown)
{
	int err;
	if (sockClientfd <= 0)       return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mouse_middle;
	tx.head.rand = rand();
	softmouse.button = (isdown ? (softmouse.button | 0x04) : (softmouse.button & (~0x04)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mouse_right(int isdown)
{
	int err;
	if (sockClientfd <= 0)       return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mouse_right;
	tx.head.rand = rand();
	softmouse.button = (isdown ? (softmouse.button | 0x02) : (softmouse.button & (~0x02)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mouse_wheel(int wheel)
{
	int err;
	if (sockClientfd <= 0)       return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mouse_wheel;
	tx.head.rand = rand();
	softmouse.wheel = wheel;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.wheel = 0;
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mouse_all(int button, int x, int y, int wheel)
{
	int err;
	if (sockClientfd <= 0)       return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mouse_wheel;
	tx.head.rand = rand();
	softmouse.button = button;
	softmouse.x = x;
	softmouse.y = y;
	softmouse.wheel = wheel;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.x = 0;
	softmouse.y = 0;
	softmouse.wheel = 0;
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mouse_move_auto(int x, int y, int ms)
{
	int err;
	if (sockClientfd <= 0)       return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mouse_automove;
	tx.head.rand = ms;
	softmouse.x = x;
	softmouse.y = y;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.x = 0;
	softmouse.y = 0;
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mouse_move_beizer(int x, int y, int ms, int x1, int y1, int x2, int y2)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_bazerMove;
	tx.head.rand = ms;
	softmouse.x = x;
	softmouse.y = y;
	softmouse.point[0] = x1;
	softmouse.point[1] = y1;
	softmouse.point[2] = x2;
	softmouse.point[3] = y2;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.x = 0;
	softmouse.y = 0;
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_keydown(int vk_key)
{
	int i;
	if (vk_key >= KEY_LEFTCONTROL && vk_key <= KEY_RIGHT_GUI)
	{
		switch (vk_key)
		{
		case KEY_LEFTCONTROL: softkeyboard.ctrl |= BIT0; break;
		case KEY_LEFTSHIFT:   softkeyboard.ctrl |= BIT1; break;
		case KEY_LEFTALT:     softkeyboard.ctrl |= BIT2; break;
		case KEY_LEFT_GUI:    softkeyboard.ctrl |= BIT3; break;
		case KEY_RIGHTCONTROL:softkeyboard.ctrl |= BIT4; break;
		case KEY_RIGHTSHIFT:  softkeyboard.ctrl |= BIT5; break;
		case KEY_RIGHTALT:    softkeyboard.ctrl |= BIT6; break;
		case KEY_RIGHT_GUI:   softkeyboard.ctrl |= BIT7; break;
		}
	}
	else
	{
		for (i = 0; i < 10; i++)
		{
			if (softkeyboard.button[i] == vk_key)
				goto KM_down_send;
		}
		for (i = 0; i < 10; i++)
		{
			if (softkeyboard.button[i] == 0)
			{
				softkeyboard.button[i] = vk_key;
				goto KM_down_send;
			}
		}
		memcpy(&softkeyboard.button[0], &softkeyboard.button[1], 10);
		softkeyboard.button[9] = vk_key;
	}
KM_down_send:
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_keyboard_all;
	tx.head.rand = rand();
	memcpy(&tx.cmd_keyboard, &softkeyboard, sizeof(soft_keyboard_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_keyboard_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_keyup(int vk_key)
{
	int i;
	if (vk_key >= KEY_LEFTCONTROL && vk_key <= KEY_RIGHT_GUI)
	{
		switch (vk_key)
		{
		case KEY_LEFTCONTROL: softkeyboard.ctrl &= ~BIT0; break;
		case KEY_LEFTSHIFT:   softkeyboard.ctrl &= ~BIT1; break;
		case KEY_LEFTALT:     softkeyboard.ctrl &= ~BIT2; break;
		case KEY_LEFT_GUI:    softkeyboard.ctrl &= ~BIT3; break;
		case KEY_RIGHTCONTROL:softkeyboard.ctrl &= ~BIT4; break;
		case KEY_RIGHTSHIFT:  softkeyboard.ctrl &= ~BIT5; break;
		case KEY_RIGHTALT:    softkeyboard.ctrl &= ~BIT6; break;
		case KEY_RIGHT_GUI:   softkeyboard.ctrl &= ~BIT7; break;
		}
	}
	else
	{
		for (i = 0; i < 10; i++)
		{
			if (softkeyboard.button[i] == vk_key)
			{
				memcpy(&softkeyboard.button[i], &softkeyboard.button[i + 1], 10 - i);
				softkeyboard.button[9] = 0;
				goto KM_up_send;
			}
		}
	}
KM_up_send:
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_keyboard_all;
	tx.head.rand = rand();
	memcpy(&tx.cmd_keyboard, &softkeyboard, sizeof(soft_keyboard_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_keyboard_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_reboot(void)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_reboot;
	tx.head.rand = rand();
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	WSACleanup();
	sockClientfd = -1;
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);

}

static void stopMonitor()
{
    monitor_run.store(monitor_exit);
    if (monitor_thread_handle) {
        // recvfrom has a 200 ms timeout; wait before reusing the socket/state.
        WaitForSingleObject(monitor_thread_handle, INFINITE);
        CloseHandle(monitor_thread_handle);
        monitor_thread_handle = nullptr;
    }
    if (sockMonitorfd != 0 && sockMonitorfd != INVALID_SOCKET) closesocket(sockMonitorfd);
    sockMonitorfd = 0;
    std::lock_guard<std::mutex> lock(monitor_state_mutex);
    hw_mouse = {};
    hw_keyboard = {};
    monitor_has_report = false;
}

DWORD WINAPI ThreadListenProcess(LPVOID)
{
    const SOCKET socket = sockMonitorfd;
    while (monitor_run.load() == monitor_ok) {
        sockaddr_in peer{};
        int peerSize = sizeof(peer);
        char buffer[1024];
        const int count = recvfrom(socket, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&peer), &peerSize);
        if (count >= static_cast<int>(sizeof(hw_mouse) + sizeof(hw_keyboard)) &&
            peer.sin_addr.s_addr == addrSrv.sin_addr.s_addr) {
            std::lock_guard<std::mutex> lock(monitor_state_mutex);
            memcpy(&hw_mouse, buffer, sizeof(hw_mouse));
            memcpy(&hw_keyboard, buffer + sizeof(hw_mouse), sizeof(hw_keyboard));
            monitor_has_report = true;
        } else if (count == SOCKET_ERROR && WSAGetLastError() != WSAETIMEDOUT) {
            break;
        }
    }
    monitor_run.store(monitor_exit);
    return 0;
}

int kmNet_monitor(short port)
{
    if (sockClientfd == 0 || sockClientfd == INVALID_SOCKET) return err_creat_socket;
    stopMonitor();
    const auto unsignedPort = static_cast<unsigned short>(port);
    if (unsignedPort != 0) {
        sockMonitorfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockMonitorfd == INVALID_SOCKET) {
            last_socket_error = WSAGetLastError();
            sockMonitorfd = 0;
            return err_creat_socket;
        }
        DWORD timeout = 200;
        BOOL exclusive = TRUE;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = htons(unsignedPort);
        if (setsockopt(sockMonitorfd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0 ||
            setsockopt(sockMonitorfd, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0 ||
            bind(sockMonitorfd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            last_socket_error = WSAGetLastError();
            stopMonitor();
            return err_net_monitor_bind;
        }
    }
    tx.head.indexpts++;
    tx.head.cmd = cmd_monitor;
    tx.head.rand = unsignedPort ? static_cast<unsigned int>(unsignedPort) | 0xaa550000u : 0;
    if (sendto(sockClientfd, reinterpret_cast<const char*>(&tx), sizeof(cmd_head_t), 0,
               reinterpret_cast<sockaddr*>(&addrSrv), sizeof(addrSrv)) != sizeof(cmd_head_t)) {
        last_socket_error = WSAGetLastError();
        stopMonitor();
        return err_net_tx;
    }
    sockaddr_in peer{};
    int peerSize = sizeof(peer);
    const int count = recvfrom(sockClientfd, reinterpret_cast<char*>(&rx), sizeof(rx), 0,
        reinterpret_cast<sockaddr*>(&peer), &peerSize);
    const int result = count < 0 ? err_net_rx_timeout :
        count < static_cast<int>(sizeof(cmd_head_t)) ? err_net_cmd : NetRxReturnHandle(&rx, &tx);
    if (result != 0) { stopMonitor(); return result; }
    if (unsignedPort != 0) {
        monitor_run.store(monitor_ok);
        monitor_thread_handle = CreateThread(nullptr, 0, ThreadListenProcess, nullptr, 0, nullptr);
        if (!monitor_thread_handle) { stopMonitor(); return err_creat_socket; }
    }
    return 0;
}

static int monitorMouse(unsigned char mask)
{
    std::lock_guard<std::mutex> lock(monitor_state_mutex);
    if (monitor_run.load() != monitor_ok || !monitor_has_report) return -1;
    return (hw_mouse.buttons & mask) ? 1 : 0;
}
int kmNet_monitor_mouse_left() { return monitorMouse(0x01); }
int kmNet_monitor_mouse_middle() { return monitorMouse(0x04); }
int kmNet_monitor_mouse_right() { return monitorMouse(0x02); }
int kmNet_monitor_mouse_side1() { return monitorMouse(0x08); }
int kmNet_monitor_mouse_side2() { return monitorMouse(0x10); }

int kmNet_monitor_keyboard(short vkey)
{
    std::lock_guard<std::mutex> lock(monitor_state_mutex);
    if (monitor_run.load() != monitor_ok || !monitor_has_report) return -1;
    const unsigned char key = static_cast<unsigned char>(vkey);
    if (key == 0) return 0;
    if (key >= KEY_LEFTCONTROL && key <= KEY_RIGHT_GUI)
        return (hw_keyboard.buttons & (1u << (key - KEY_LEFTCONTROL))) ? 1 : 0;
    for (const auto down : hw_keyboard.data) if (down == key) return 1;
    return 0;
}

int kmNet_debug(short port, char enable)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_debug;
	tx.head.rand = port | enable << 16;
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);

}

int kmNet_mask_mouse_left(int enable)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mask_mouse;
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT0) : (mask_keyboard_mouse_flag &= ~BIT0);
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mask_mouse_right(int enable)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mask_mouse;
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT1) : (mask_keyboard_mouse_flag &= ~BIT1);
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mask_mouse_middle(int enable)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mask_mouse;
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT2) : (mask_keyboard_mouse_flag &= ~BIT2);
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mask_mouse_side1(int enable)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mask_mouse;
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT3) : (mask_keyboard_mouse_flag &= ~BIT3);
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mask_mouse_side2(int enable)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mask_mouse;
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT4) : (mask_keyboard_mouse_flag &= ~BIT4);
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mask_mouse_x(int enable)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mask_mouse;
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT5) : (mask_keyboard_mouse_flag &= ~BIT5);
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mask_mouse_y(int enable)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mask_mouse;
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT6) : (mask_keyboard_mouse_flag &= ~BIT6);
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mask_mouse_wheel(int enable)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mask_mouse;
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT7) : (mask_keyboard_mouse_flag &= ~BIT7);
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_mask_keyboard(short vkey)
{
	int err;
	BYTE v_key = vkey & 0xff;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_mask_mouse;
	tx.head.rand = (mask_keyboard_mouse_flag & 0xff) | (v_key << 8);
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_unmask_keyboard(short vkey)
{
	int err;
	BYTE v_key = vkey & 0xff;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_unmask_all;
	tx.head.rand = (mask_keyboard_mouse_flag & 0xff) | (v_key << 8);
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_unmask_all()
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_unmask_all;
	mask_keyboard_mouse_flag = 0;
	tx.head.rand = mask_keyboard_mouse_flag;
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_setconfig(char* ip, unsigned short port)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	tx.head.indexpts++;
	tx.head.cmd = cmd_setconfig;
	tx.head.rand = inet_addr(ip);
	tx.u8buff.buff[0] = port >> 8;
	tx.u8buff.buff[1] = port >> 0;
	int length = sizeof(cmd_head_t) + 2;
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = recvfrom(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_lcd_color(unsigned short rgb565)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	for (int y = 0; y < 40; y++)
	{
		tx.head.indexpts++;
		tx.head.cmd = cmd_showpic;
		tx.head.rand = 0 | y * 4;
		for (int c = 0; c < 512; c++)
			tx.u16buff.buff[c] = rgb565;
		int length = sizeof(cmd_head_t) + 1024;
		sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
		SOCKADDR_IN sclient;
		int clen = sizeof(sclient);
		err = recvfrom(sockClientfd, (char*)&rx, length, 0, (struct sockaddr*)&sclient, &clen);
		if (err < 0)
			return err_net_rx_timeout;
	}
	return NetRxReturnHandle(&rx, &tx);

}

int kmNet_lcd_picture_bottom(unsigned char* buff_128_80)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	for (int y = 0; y < 20; y++)
	{
		tx.head.indexpts++;
		tx.head.cmd = cmd_showpic;
		tx.head.rand = 80 + y * 4;
		memcpy(tx.u8buff.buff, &buff_128_80[y * 1024], 1024);
		int length = sizeof(cmd_head_t) + 1024;
		sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
		SOCKADDR_IN sclient;
		int clen = sizeof(sclient);
		err = recvfrom(sockClientfd, (char*)&rx, length, 0, (struct sockaddr*)&sclient, &clen);
		if (err < 0)
			return err_net_rx_timeout;
	}
	return NetRxReturnHandle(&rx, &tx);
}

int kmNet_lcd_picture(unsigned char* buff_128_160)
{
	int err;
	if (sockClientfd <= 0)        return err_creat_socket;
	for (int y = 0; y < 40; y++)
	{
		tx.head.indexpts++;
		tx.head.cmd = cmd_showpic;
		tx.head.rand = y * 4;
		memcpy(tx.u8buff.buff, &buff_128_160[y * 1024], 1024);
		int length = sizeof(cmd_head_t) + 1024;
		sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
		SOCKADDR_IN sclient;
		int clen = sizeof(sclient);
		err = recvfrom(sockClientfd, (char*)&rx, length, 0, (struct sockaddr*)&sclient, &clen);
		if (err < 0)
			return err_net_rx_timeout;
	}
	return NetRxReturnHandle(&rx, &tx);
}
