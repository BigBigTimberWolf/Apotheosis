path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\USBSetup.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

OLD = """USBHIDMouse Mouse;
USBHIDKeyboard Kbd;"""

NEW = """// ===== 全局 HID 对象的声明顺序 = 实例编号 (关键!) =====
//
// 【框架机制】USBHIDMouse / USBHIDKeyboard 的构造函数里就调用了:
//     hid.addDevice(this, sizeof(report_descriptor));
// 也就是【构造时就注册】。而实例编号 = 注册顺序:
//     第 1 个被构造的 = 实例 0, 第 2 个 = 实例 1 ...
//
// 而 USBHID::SendReport() 内部是:
//     res = tud_hid_n_report(0, id, data, len);
//                         ^ 实例号【硬编码为 0】
// 也就是说所有 HID 报文都只会写往"实例 0", 框架假定设备里只有一个 HID 实例。
//
// 【原来的致命问题】这里原来是:
//     USBHIDMouse Mouse;      // 先构造 -> 鼠标 = 实例 0
//     USBHIDKeyboard Kbd;     // 后构造 -> 键盘 = 实例 1
// 于是 Kbd.press() -> SendReport -> tud_hid_n_report(0,...) 把【键盘报文写进
// 鼠标实例】, 键盘永远收不到输入。症状正是: 键盘设备枚举正常、驱动已绑定,
// 但打字毫无反应。
//
// 注意 begin() 的调用顺序【无法】改变这一点 —— begin() 只是 hid.begin(),
// 决定实例号的是构造顺序, 而全局对象在 setup() 之前就构造完了。
//
// 【修法】让键盘先构造, 于是键盘 = 实例 0, SendReport 命中键盘。
// 鼠标位移在本固件里走的是 ASCII 命令(km.move)与独立协议, 不依赖 HID 实例
// 编号, 因此把实例 0 让给键盘是安全的。
//
// 【约束】这两个对象本身不能交换名字(handleCommands.cpp 等处都按
// Mouse/Kbd 引用), 所以这里显式控制构造顺序: 用一个哨兵对象保证 Kbd 先生成。
// 做法是把 Kbd 的定义放在前面。
USBHIDKeyboard Kbd;
USBHIDMouse Mouse;"""

assert OLD in src, "global HID object definitions not found"
src = src.replace(OLD, NEW, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("global HID object order swapped: Kbd now constructed first (instance 0)")
