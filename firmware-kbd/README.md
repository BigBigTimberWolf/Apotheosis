# 键盘透传 (KBD_PASSTHROUGH)

**状态: 已验证可用 (能正常打字)**

---

## 这是什么

把真键盘的 USB 报文**原样**透传到被控机:

```
真键盘 --USB--> 右板(USB Host) --Serial1--> 左板(USB Device) --USB--> 被控机
                 只搬字节, 不解析              补个 Report ID
```

**设计原则: 不解析就不可能解析错。**

老做法要「解析描述符 → 判断键盘/鼠标 → 拆修饰键和键码 → 重组」,
中间任何一步判断错键盘就不工作。现在直接搬运原始字节,
多媒体键、组合键、厂商自定义键全部天然保留。

---

## 硬件接线

| 板子 | 接口 | 接到 | 说明 |
|------|------|------|------|
| 右板 | USB-C | 真键盘 | Host 口 |
| 右板 | USB-B | CH343 | Serial, 4000000 波特率(日志) |
| 左板 | USB-A | 被控机 | Device 口 |
| 左板 | USB-B | CH343 | Serial, **115200**(日志) |
| 板间 | Serial1 | 互连 | 5000000 8N1 |

**Serial1 引脚是交叉的:**
- 左板: `Serial1.begin(5000000, SERIAL_8N1, 1, 2)`  (RX=1, TX=2)
- 右板: `Serial1.begin(5000000, SERIAL_8N1, 2, 1)`  (RX=2, TX=1)

**上电顺序: 先插 USB-C(键盘), 再插 USB-A(通电)** —— 见下方"身份"说明。

---

## 两个工程

| 目录 | 芯片 | 产出 |
|------|------|------|
| `fw_host_kbd/` | 右板 | `Right_MAKCM_MCU_MCU_KBD_PT_V1_0.bin` |
| `fw_device_kbd/` | 左板 | `Left_MAKCM_MCU_MCU_KBD_PT_V1_0.bin` |

**必须两个一起刷, 不能混用旧版本。**

---

## 关键设计决策 (踩过的坑)

### 1. 为什么不用两个 HID 接口

预编译的 TinyUSB 库写死了 `CFG_TUD_HID=1`:

```
符号 _hidd_itf 总大小 = 140 字节
tud_hid_n_report 里 instance 步长 = 140 字节
=> 只放得下 1 个 HID 实例
```

工程里写 `-DCFG_TUD_HID=2` **改不了已编译好的 `.a`**。第二个 HID 接口会被
`hidd_open()` 拒绝, 导致**整个配置枚举失败**(连鼠标一起死)。

**采用方案: 单接口 + 双 Top-Level Collection**

Windows 的 `kbdhid`/`mouhid` 是按 **Collection** 加载的, 不是按接口,
所以一个接口里放两个 Collection 同样能被分别识别:

```
接口 0: Class_03 / SubClass_01 / Prot_01 (Boot Keyboard)
   Report ID 1 -> 键盘
   Report ID 2 -> 鼠标
bDeviceClass = 0x00  -> Windows 加载 usbccgp
VID:PID = 3554:FA09
```

### 2. 为什么能用自己的描述符

Arduino 核心里这三个回调是 `__attribute__((weak))`:

```
esp32-hal-tinyusb.c:270  tud_descriptor_configuration_cb
esp32-hal-tinyusb.c:279  tud_descriptor_device_cb
esp32-hal-tinyusb.c:288  tud_descriptor_string_cb
```

我们在 `usb_desc.cpp` 里给**强定义**直接覆盖, **不需要 `--wrap`**。

同时必须 `-DARDUINO_USB_CDC_ON_BOOT=0`, 否则 Arduino 会自建 USB 栈抢走枚举。

### 3. 帧格式 (板间协议)

见 `shared/kbd_wire.h`, 详细注释在那里。

```
下标 0..4          : A5 5C len seq type
下标 5..4+plen     : payload
下标 5+plen..6+plen: CRC16/MODBUS (低字节在前)
总长 = plen + 7
```

**帧类型:**
| 类型 | 方向 | payload |
|------|------|---------|
| `0x23` | 右→左 | 键盘原始报文 (8 字节) |
| `0x24` | 左→右 | LED 状态 (1 字节) |
| `0x25` | 右→左 | 设备身份 VID/PID/bcd (6 字节) |

**★ 曾经在这里犯过 off-by-one:** CRC 位置写成 `crcStart+crcLen`
(= `plen+4`), 正确的是 `5+plen`。差了 1 字节, CRC 覆盖了 payload 尾部,
导致左板 `frames=0`, `crc` 一直涨。**改协议后务必跑 `test_wire.py`。**

### 4. 设备身份是动态的

被控机在**枚举那一刻**就要读到 VID/PID, 那时键盘可能还没插。
解决办法:

```
上电 -> 右板枚举真键盘, 读到描述符
     -> 右板发身份帧(0x25)给左板
     -> 左板用真值填描述符
     -> 才调 tinyusb_init() 开始枚举
```

左板**最多等 1.5 秒**, 等不到就用默认 `3554:FA09` —— **不会变砖**。
这就是为什么要「先插键盘再通电」。

---

## 验证方法

### 协议层 (不需要硬件)

```bash
python test_wire.py     # 帧打包/解包往返, 各长度
python test_wire2.py    # LED 帧 + 身份帧 + 类型隔离
python parse_desc.py    # 报告描述符结构校验 (Collection/Report ID)
```

### 设备侧

插上被控机后, 设备管理器应该看到:

```
USB\VID_3554&PID_FA09\...
   ├─ COL01 -> Service = kbdhid    ("HID Keyboard Device")
   └─ COL02 -> Service = MouHid
```

父设备 `ProblemCode = 0` 表示无问题。

### 串口日志

接 CH343 到**左板** (115200):

```
[KBD-DEV] usb=READY frames=N sent=N drop=0 crc=0 led=N
```

- `frames` / `sent` 随打字增长 = 链路通
- `crc` 增长 = 帧格式不匹配, 检查 `kbd_wire.h`
- `drop` 增长 = USB 没就绪, 报文被丢

接 CH343 到**右板** (4000000):

```
[KBD-HOST] reports=N frames=N id=N led=N
```

---

## 已知限制

| 项 | 状态 | 说明 |
|----|------|------|
| **LED 指示灯** | ⚠️ 不正常 | `led_forward.cpp` 里的 `SET_REPORT` 是离线写的, 没验证成功。**不影响打字**, 用户表示不需要 |
| **轮询间隔** | 未透传 | 左板用 1ms。真要透传需重新枚举, 代价大收益小 |
| **序列号** | 固定 | `000000000001`, 插不同键盘被控机看到同一个设备 |
| **鼠标** | 未接入 | 左板的鼠标 Collection 已预留, 但链路只做了键盘 |

---

## 排查经验

**最重要的教训: 先确认链路通不通, 再优化结构。**

之前反复失败是因为同时改左右两侧、没有日志, 无法定位。
后来接上 CH343 看到 `crc=9`, **一秒就定位了 off-by-one**。

**出问题时先看日志的 `frames`/`crc`/`drop` 三个数, 不要先改代码。**
