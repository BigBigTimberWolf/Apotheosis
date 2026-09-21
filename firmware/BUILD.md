# 本工作区的编译说明 (Windows / 离线)

## 快速编译

```powershell
.\build.ps1            # 编译两个固件
.\build.ps1 host       # 只编译 fw_host (RIGHT)
.\build.ps1 device     # 只编译 fw_device (LEFT)
```

编译产物:
- `fw_host\.pio\build\RIGHT\firmware.bin`
- `fw_device\.pio\build\LEFT\firmware.bin`

---

## 为什么需要这套东西

本机环境的三个限制导致 PlatformIO 无法开箱即用, 仓库里因此带了 `piocore\`
(工具链) 和 `pioinstall.py` / `_pio_setup.py` (包安装器):

1. **网络代理不可用** —— 官方 `pip install platformio` 走不通。
   仓库内自带 `.piopkgs\` (PlatformIO 6.2.0), 直接 `PYTHONPATH` 引用即可。

2. **PlatformIO 自带的解包器在本沙箱内必然失败**
   (`WinError 5 拒绝访问` on `bin\` / `LICENSE` / `.github\`)。
   但压缩包下载是正常的, 用 Python 的 `tarfile` 解包 1600+ 文件零失败。
   所以流程改为: 让 PlatformIO 负责下载 → 我们自己解包 → 补写 `.piopm`
   元数据标记, PlatformIO 便会认为包已安装。

3. **响应文件 (response file) 触发 Windows 命令行长度上限** —— 这是最隐蔽的一条:
   - 本项目有 200+ 个 `-I`, 且工作区路径较深, 展开后约 **32KB**;
   - 该 xtensa g++ 读 `@响应文件` 后会把整条展开的命令行再交给 `cc1plus`;
   - Windows `CreateProcess` 上限是 **32767 字符**, 于是每次编译必报
     `xtensa-esp32s3-elf-g++: error: CreateProcess: No such file or directory`。

   解决: 在写响应文件时把 `-I` / `-L` 的绝对路径改写为 **8.3 短路径**
   (每条省约 40 字符, 32KB → 23KB)。
   注意 **只改 `-I`/`-L` 目录, 绝不改源文件名** —— 8.3 会把扩展名大写,
   `esp32-hal-adc.c` 变成 `ESP32-~1.C`, GCC 会当成 C++ 编译, 整个 Arduino
   核心全崩。

---

## 打过的补丁 (都在 .piopkgs\ 和 piocore\ 内, 非固件代码)

| 文件 | 作用 |
| --- | --- |
| `.piopkgs\platformio\package\unpack.py` | 跳过点文件/点目录, 并让解包后的存在性校验同步跳过 |
| `.piopkgs\platformio\builder\tools\piomaxlen.py` | 响应文件内 `-I`/`-L` 改写为 8.3 短路径 (关键修复) |
| `piocore\packages\tool-scons\...\SCons\Platform\__init__.py` | 备用: 未配置转义函数时, Windows 下反斜杠改正斜杠 |

---

## 重新安装工具链

若 `piocore\` 被删除:

```powershell
python pioinstall.py          # 触发下载
python _pio_setup.py          # 解包 + 写 .piopm 标记
python _set_owners.py         # 补 owner 字段 (否则 PlatformIO 认不出, 会重装)
```

`_set_owners.py` 不可省略: PlatformIO 用 `owner/name` 查找包, `owner` 为
`null` 时查不到, 就会重新下载 + 重新解包, 再次撞上上面第 2 条。

---

## 固件改动说明 (本轮)

- **复合 HID 端点归属改为按端点索引**: `ep_owner[32]`, 下标含方向位,
  修掉只按端点号索引导致 IN/OUT 互相覆盖的问题。
- **每个接口各自保存报文布局**: `descPerIface[8]`, 修掉单个全局描述符
  被复合设备的多个接口互相覆盖的问题。
- **接口号经 `transfer->context` 传到控制回调** (`ControlContext`):
  `usb_transfer_s` 里没有 `bInterval` 字段, 不能用它携带接口号。
- **修复 `interface_descriptors[]` 从未被写入** (主机侧), 之前
  `sendInterfaceDescriptors()` 发出的全是 0。
- **补上 `fw_device` 的 `Kbd.begin()`**: 键盘接口此前从未注册,
  `Kbd.press/release` 调用等于空操作。
- **代码瘦身**: 合并 10 个鼠标按键孪生处理函数为一个
  `handleKmMouseButton()`; 移除死宏/死结构体; `logRawBytes()` 改为
  零堆分配的 `snprintf` 实现。

详见 `docs/proto.md`。
