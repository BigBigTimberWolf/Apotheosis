#ifndef KMBOX_NET_H
#define KMBOX_NET_H
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <stdio.h>

#include <cmath>
#pragma warning(disable : 4996)

#define     cmd_connect         0xaf3c2828 // ok: Connect to the box
#define     cmd_mouse_move      0xaede7345 // ok: Mouse move
#define     cmd_mouse_left      0x9823AE8D // ok: Mouse left button control
#define     cmd_mouse_middle    0x97a3AE8D // ok: Mouse middle button control
#define     cmd_mouse_right     0x238d8212 // ok: Mouse right button control
#define     cmd_mouse_wheel     0xffeead38 // ok: Mouse wheel control
#define     cmd_mouse_automove  0xaede7346 // ok: Simulated human mouse movement control
#define     cmd_keyboard_all    0x123c2c2f // ok: Control all keyboard parameters
#define     cmd_reboot          0xaa8855aa // ok: Box reboot
#define     cmd_bazerMove       0xa238455a // ok: Mouse Bezier movement
#define     cmd_monitor         0x27388020 // ok: Monitor physical mouse/keyboard data on the box
#define     cmd_debug           0x27382021 // ok: Enable debug info
#define     cmd_mask_mouse      0x23234343 // ok: Block physical mouse/keyboard
#define     cmd_unmask_all      0x23344343 // ok: Unblock physical mouse/keyboard
#define     cmd_setconfig       0x1d3d3323 // ok: Set IP configuration info
#define     cmd_showpic         0x12334883 // ok: Display image

extern SOCKET sockClientfd;
typedef struct
{
	unsigned int  mac;
	unsigned int  rand;
	unsigned int  indexpts;
	unsigned int  cmd;
} cmd_head_t;

typedef struct
{
	unsigned char buff[1024];
}cmd_data_t;
typedef struct
{
	unsigned short buff[512];
}cmd_u16_t;

typedef struct
{
    int button;
    int x;
    int y;
    int wheel;
    int point[10];
} soft_mouse_t;

typedef struct
{
    char ctrl;
    char resvel;
    char button[10];
} soft_keyboard_t;

typedef struct
{
    cmd_head_t head;
    union {
        cmd_data_t      u8buff;
        cmd_u16_t       u16buff;
        soft_mouse_t    cmd_mouse;
        soft_keyboard_t cmd_keyboard;
    };
} client_tx;

enum
{
    err_creat_socket = -9000,
    err_net_version,
    err_net_tx,
    err_net_rx_timeout,
    err_net_cmd,
    err_net_pts,
    success = 0,
    usb_dev_tx_timeout,
};

int kmNet_init(char* ip, char* port, char* mac);
int kmNet_mouse_move(short x, short y);
int kmNet_mouse_left(int isdown);
int kmNet_mouse_right(int isdown);
int kmNet_mouse_middle(int isdown);
int kmNet_mouse_wheel(int wheel);
int kmNet_mouse_all(int button, int x, int y, int wheel);
int kmNet_mouse_move_auto(int x, int y, int time_ms);
int kmNet_mouse_move_beizer(int x, int y, int ms, int x1, int y1, int x2, int y2);

int kmNet_keydown(int vkey);
int kmNet_keyup(int vkey);

int kmNet_monitor(short port);
int kmNet_monitor_mouse_left();
int kmNet_monitor_mouse_middle();
int kmNet_monitor_mouse_right();
int kmNet_monitor_mouse_side1();
int kmNet_monitor_mouse_side2();
int kmNet_monitor_keyboard(short vk_key);

int kmNet_mask_mouse_left(int enable);
int kmNet_mask_mouse_right(int enable);
int kmNet_mask_mouse_middle(int enable);
int kmNet_mask_mouse_side1(int enable);
int kmNet_mask_mouse_side2(int enable);
int kmNet_mask_mouse_x(int enable);
int kmNet_mask_mouse_y(int enable);
int kmNet_mask_mouse_wheel(int enable);
int kmNet_mask_keyboard(short vkey);
int kmNet_unmask_keyboard(short vkey);
int kmNet_unmask_all();

int kmNet_reboot(void);
int kmNet_setconfig(char* ip, unsigned short port);
int kmNet_debug(short port, char enable);
int kmNet_lcd_color(unsigned short rgb565);
int kmNet_lcd_picture_bottom(unsigned char* buff_128_80);
int kmNet_lcd_picture(unsigned char* buff_128_160);
#endif // KMBOX_NET_H
