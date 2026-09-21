#include "EspUsbHost.h"
#include "efuse.h"
#include "diag.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#if USB_IS_DEBUG
    #warning "DEBUG MODE ENABLED: USB host will not work! For logging purposes only."
#endif

#ifndef FIRMWARE_VERSION
    #error "FIRMWARE_VERSION is not defined! Please set FIRMWARE_VERSION in the build flags."
#endif

const char* firmware = TOSTRING(FIRMWARE_VERSION);

EspUsbHost usbHost;

void setup()
{
#if FW_BOOTTEST
  // 最小启动自检: 不碰 USB / Serial1, 只用 LED9 闪灯。
  // 目的是把"固件没跑起来"与"USB 逻辑有问题"彻底分开。
  bootTestStart();     // 内部是死循环, 不会返回
  return;
#endif

#if FW_USBSTAGE
  // USB 分级诊断: 逐级初始化, 用 LED9 脉冲个数报告卡在哪一级。
  usbStageStart();     // 内部是死循环, 不会返回
  return;
#endif

  Serial0.begin(4000000);
  Serial1.begin(5000000, SERIAL_8N1, 2, 1); // Swap RX/TX from ESP A
  delay(1000);
  pinMode(9, OUTPUT);
  usbHost.begin();
  Serial0.println("RIGHT: MCU Started");
  Serial1.println("MAKCK v1.2");
#if !FW_DIAG
  // 诊断构建绝不烧这个一次性不可逆的 efuse: 它决定 S3 唯一的 USB PHY 给
  // Device 还是 Host, 烧错方向不可逆。诊断固件只把当前值读出来上报
  // (见 diag.cpp), 让我们先看清它到底是不是右板 Host 起不来的原因。
  burn_usb_phy_sel_efuse();
#endif

#if FW_DIAG
  diagStart();
#endif
}

void loop()
{
  // Clean
}
