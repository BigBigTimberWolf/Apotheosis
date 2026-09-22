# MAKCUNEW 固件源码

## 两个固件分别是哪个

| 你要的 | PlatformIO env | 源码目录 | 产物文件名 |
|---|---|---|---|
| **USB-A(左)** | `LEFT` | `fw_device/` | `Left_MAKCM_MCU_CUSTOM_V1_0.bin` |
| **USB-C(右)** | `RIGHT` | `fw_host/` | `Right_MAKCM_MCU_V1_2.bin` |

两块板子都是 ESP32-S3(MAKCM_MCU,4MB flash,240MHz)。
`fw_device` 是**从机**(作为 USB 设备把鼠标动作送给游戏机),
`fw_host` 是**主机**(作为 USB host 读真实鼠标)。

## 怎么编译

```bash
cd fw_device && pio run -e LEFT      # 出 USB-A(左)
cd fw_host   && pio run -e RIGHT     # 出 USB-C(右)
```

产物落在各自的 `merged_firmware/` 目录里。

两个 bin 都是 **合并镜像**(bootloader @0x0 + 分区表 @0x8000 + 应用 @0x10000),
所以烧录直接从 **0x0** 开始,不需要分别烧三个文件:

```bash
esptool.py --chip esp32s3 write_flash 0x0 Left_MAKCM_MCU_CUSTOM_V1_0.bin
```

## 版本号

- USB-A(左):`CUSTOM_V1_0`
- USB-C(右):`V1_2`

版本号来自各自的 `platformio.ini` 里的 `-DFIRMWARE_VERSION=`,
文件名由 `scripts/merge.py` 自动拼出来。

## ⚠️ 首次编译会很慢(踩过的坑)

PlatformIO 的包 CDN 在国内实测只有 **35~60 KB/s**,
而 `framework-arduinoespressif32` 有 **234.9 MB** —— 直连要下一个半小时到两小时。

### 加速办法:手动下载再塞进缓存

真实 CDN 地址(从 registry API 拿的):

```
https://dl.registry.platformio.org/download/platformio/tool/framework-arduinoespressif32/3.20016.0/framework-arduinoespressif32-3.20016.0.tar.gz
```

它会 302 跳转到 `dl.registry.ns3.platformio.org`。
**挂代理 + 多线程分块下载**能跑到 0.4~8 MB/s。

下完之后**不需要**放进 PlatformIO 的缓存目录,
直接解包到 `~/.platformio/packages/` 下对应名字的目录即可:

```bash
mkdir -p ~/.platformio/packages/framework-arduinoespressif32
tar -xzf framework-arduinoespressif32-3.20016.0.tar.gz \
    -C ~/.platformio/packages/framework-arduinoespressif32
```

PlatformIO 会读目录里的 `package.json` 里的 `version` 字段,
只要版本满足 `platform.json` 里的 `~3.20016.0` 就会认为已安装,跳过下载。

其余几个小包同理(`tool-esptoolpy` 0.49 MB、`toolchain-xtensa-esp32s3`)。

### 不要做的事

- **不要**把 Arduino IDE 里的 ESP32 core 软链进来。
  Arduino IDE 装的是 **3.3.8**(Arduino core 3.x),
  而这里的 `~3.20016.0` 对应的是 **Arduino core 2.0.16** —— 差一个大版本,
  ESP-IDF 和 HAL 都不一样,编不过。
- 版本号编码规则:PlatformIO 的 `3.20016.0` = Arduino core **2.0.16**。

## 目录结构

```
fw_device/          USB-A(左),env LEFT
  src/              应用源码
  include/          头文件
  boards/           板级定义 (MAKCM.json)
  partitions/       分区表
  scripts/merge.py  合并 bootloader+分区表+应用到单个 bin
  platformio.ini

fw_host/            USB-C(右),env RIGHT
  (结构同上)

docs/
  proto.md          MAKCU 协议说明
  makcu_proto.h     协议头文件
```

## 协议要点(备忘)

主机↔设备走 USB CDC,帧格式有两种:

1. `A5 5C | LEN | SEQ | CMD | PAYLOAD | CRC16`
2. `DE AD | LEN_LO LEN_HI | CMD | PAYLOAD`

第二种的 CMD `0xA5` = 设置波特率。
**6 Mbps 的完整命令是:`DE AD 05 00 A5 80 8D 5B 00`**

## ⚠️ 按键上报:两条独立通道(上位机联调必读)

固件 `fw_device/src/handleCommands.cpp` 的 `updateButtonState()` 内有**两个互不相干的开关**,
开一个不会开另一个:

| 通道 | 固件开关 | 上行格式 | 开启方式 |
|---|---|---|---|
| A 裸掩码流 | `g_makcu_buttons_enabled` | 单字节 `merged & 0x1F`(0..31) | ASCII:`km.buttons(1)` |
| B 二进制异步帧 | `g_async_sub` | `A5 5C` 帧,CMD `0x84`,payload = `real_mask, inj_mask` | 二进制:`CMD_SUB_ASYNC (0x48)` |

**两条通道都是「事件驱动」——只在按键状态发生变化时才推一帧,固件不会周期性补发。**

因此只要那一帧在链路上丢失(USB-CDC 抖动、CRC 校验失败、接收侧缓冲分片错位),
上位机的按键状态就会**永久停在上一个值**,固件也不会再补发,直到用户松开再按一次。
现象是「硬件连上了、鼠标能动,但所有绑定鼠标键的热键一律没反应」。

上位机的应对(见 `Apotheosis/mouse/MakcuNew.cpp`):

1. **两条通道都开**。通道 A 只需 1 个 `<32` 的字节,接收侧不依赖帧同步,抗错位能力强得多。
2. **周期性重订阅保活**。重发订阅会触发固件 `onSubAsync` 里的 `updateButtonState()`,
   强制补发当前真实按键态,从而让丢帧在数秒内自愈。
3. **启动时校验**。订阅后若限定时间内收不到任何按键帧,明确打印 WARNING,
   而不是静默失败——否则用户只能看到「热键没反应」,无从判断。

> ⚠️ 通道 A 的掩码取值 0..31 与 ASCII 控制字符重叠:`0x0A`(换行)、`0x0D`(回车)
> 都是合法的按键组合。上位机因此在 `feedAsciiByte()` 里把掩码判定提到换行判定**之前**,
> 并用「是否已开启 `km.buttons` 模式」做门控,未开启时完全退回文本语义。

## 目录补充说明

| 目录 | 说明 |
|---|---|
| `_working_baseline/` | 早期可用基线,含 `修复记录.md` |
| `_fix1_build/` | 某次修复分支的源码快照 |
| `_orig0910_build/` | 0910 版原始源码快照,用于对照差异 |
| `_backup_before_kbd_removal/` | 移除键盘功能**之前**的备份(含当时左右板 bin) |
| `_disasm/` | 反汇编片段,排查固件行为时用 |

根目录的 `*.py` 是开发期的一次性脚本(补丁生成、差分、校验、串口读取等),
保留是为了可追溯当时怎么改的,**不是构建流程的一部分**。

> **注**:`piocore/`(PlatformIO 工具链,约 4.8 GB)与各 `*/.pio/`(构建中间产物)
> 未纳入版本管理;在新机器上首次构建需重新安装工具链,见上文「首次编译会很慢」。

## 相关固件

| 目录 | 说明 |
|---|---|
| `firmware/` (本目录) | **MAKCUNEW 鼠标透传固件**。左板 `fw_device` + 右板 `fw_host`,负责鼠标位移/按键注入与真实输入屏蔽。 |
| `firmware-kbd/` | **键盘透传固件 (KBD_PASSTHROUGH)**。把真键盘的 USB 报文原样搬给被控机,含 LED 与设备身份透传。 |

两者是**独立的两套固件、各自独立刷写**,不要混用。上位机侧与之相关的
键盘能力判断见 `Apotheosis/mouse/mouse_driver.cpp` 的 `kCapKeyboard`:
只有键盘硬件确实存在时才声明该能力位,自动急停的键盘屏蔽命令也只从键盘那台发出。
