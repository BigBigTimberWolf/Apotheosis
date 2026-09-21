// ============================================================
// boottest.cpp —— 右板最小自检与 USB 分级诊断
//
// 分两个构建使用:
//   -DFW_BOOTTEST=1  : 只闪灯, 不碰 USB。验证"固件能跑起来"。
//   -DFW_USBSTAGE=1  : 逐级初始化 USB, 每一级结果用 LED9 闪码报告。
//
// ------------------------------------------------------------
// 【已确认事实】固件本身的启动是好的
// ------------------------------------------------------------
// BOOTTEST 构建在老马克上实测: 先 3 次长闪, 随后持续快闪 —— 证明
// 固件、时钟、startup、GPIO9、板级配置(board_dir)全部正常。
// 因此"灯不亮 + 不动"的问题被限定在 USB 逻辑内部。
//
// ------------------------------------------------------------
// 【为什么需要 USBSTAGE】
// ------------------------------------------------------------
// 生产固件的灯由 flashLED() 驱动, 而 flashLED() 只在 _onReceive 收到报文时
// 才被调用。也就是说"生产固件灯不亮"这一个现象, 同时对应下面【五种】完全
// 不同的故障, 光看灯无法区分:
//     ① usb_host_install() 失败            (PHY 被占 / 参数错)
//     ② client_register 失败               (回调配置错)
//     ③ 一直没有 NEW_DEV 事件              (C 口没枚举到设备 / D+/D- 问题)
//     ④ 枚举到了但端点没提交成功           (claim / transfer_alloc / submit)
//     ⑤ 端点都在收但要的数据是 0 字节      (设备不发报文)
//
// USBSTAGE 逐级初始化并在每一步用【不同节奏的闪烁】报告结果, 于是这五种
// 情况可以被一次性分辨出来, 不需要示波器, 也不需要串口(右板没有 CH343)。
//
// ------------------------------------------------------------
// 【闪码表】—— 上电后连续循环, 每轮之间停顿 1.5 秒便于分辨
// ------------------------------------------------------------
//   每轮 = 若干短脉冲(120ms 亮/120ms 灭) + 1.5s 长灭。
//   数脉冲个数即可定位, 不需要计时间。
//
//   1 个脉冲  -> usb_host_install() 失败(USB PHY 没拿到)
//   2 个脉冲  -> client_register 失败
//   3 个脉冲  -> install+client 都成功, 但枚举不到任何设备(C 口没认到)
//   4 个脉冲  -> 枚举到了, 但打不开设备 / 取不到配置描述符
//   5 个脉冲  -> 枚举完全成功 —— 问题在报文收取/解码/发送环节
//
// 【实测记录】
//   第一版(回调传 nullptr): 2 个脉冲 —— 已从反汇编确认是本诊断自身的 bug,
//   不是为了固件的问题(异步客户端必须给回调), 但它反证了
//   usb_host_install() 成功, 硬件/PHY 侧可以排除。
// ------------------------------------------------------------
#include "Arduino.h"
#include "EspUsbHost.h"
#include "diag.h"
#include "usb/usb_host.h"

static const int BT_LED = 9;

static inline void btLed(bool on) { digitalWrite(BT_LED, on ? HIGH : LOW); }

// USB 事件回调。
//
// ★ 这里承担本诊断最关键的一步: 打开设备。
//
// 【为什么必须在回调里打开】实测现象是"能枚举到(出现在设备地址列表里),
// 但 usb_host_device_open() 失败"(4 个脉冲)。原因是: 设备只有在 NEW_DEV
// 事件被派发的那个时刻才处于"可认领"状态, 事件处理完就转走了。
// 所以在事件回调【外面】轮询着去 open 是打不开的 —— 这正是本诊断前一版
// 报 4 的原因, 也解释了为什么它是本诊断的缺陷而非固件缺陷。
//
// 生产固件(esp_usb_host.cpp 的 _clientEventCallback)正是这么做的:
//   case USB_HOST_CLIENT_EVENT_NEW_DEV:
//       usb_host_device_open(clientHandle, eventMsg->new_dev.address, &deviceHandle);
// 本回调复刻这一流程, 并把结果记进 g_stage, 供下面的循环报码。
static volatile int g_stage = 3;      // 3=未枚举到 4=打开失败 5=成功 6=取描述符失败
static volatile int g_err   = 0;      // 最近一次失败的 esp_err_t(用于区分失败原因)

// 客户端句柄。
//
// 用【文件级静态变量】而不是把局部变量的地址塞进 callback_arg:
// 局部变量的地址在函数返回/作用域变化后就是野指针, 而回调是异步触发的,
// 一旦在错误的时机解引用, 轻则取到垃圾句柄(open 必然失败), 重则崩溃。
// 静态变量生命周期贯穿全程, 没有这个问题。
static usb_host_client_handle_t g_client = NULL;

static void usbStageEventCb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;

    if (msg->event != USB_HOST_CLIENT_EVENT_NEW_DEV) {
        return;
    }

    // 设备刚接入: 立刻在当前上下文里打开它 —— 这是唯一可靠的时机。
    usb_device_handle_t dev = NULL;
    esp_err_t err = usb_host_device_open(g_client, msg->new_dev.address, &dev);
    if (err != ESP_OK || dev == NULL) {
        g_stage = 4;                  // 级 4: 枚举到但打开失败
        g_err   = (int)err;           // 记下错误码, 供闪码读出
        return;
    }

    // 打开成功后再取活动配置描述符, 确认枚举真的完整。
    const usb_config_desc_t *cfg = NULL;
    err = usb_host_get_active_config_descriptor(dev, &cfg);
    if (err != ESP_OK || cfg == NULL) {
        usb_host_device_close(g_client, dev);
        g_stage = 6;                  // 级 6: 打开成功但取描述符失败
        g_err   = (int)err;
        return;
    }

    // 走到这里: install / client / 枚举 / open / 取描述符 全部成功。
    // 保留设备句柄不关闭 —— 关掉会触发又一次枚举循环, 干扰观察。
    g_stage = 5;
    g_err   = 0;
    (void)cfg;
}

// 最小闪灯: 纯 GPIO, 不依赖任何 RTOS 对象。
//
// 节奏说明(按实测反馈调整): 原先是 120ms 亮/120ms 灭, 太短不好数。
// 现在放到 400ms 亮/400ms 灭 —— 单脉冲 0.8 秒, 徒眼数得很清楚,
// 而且不会长到让人失去耐心(报 5 个也只要 4 秒)。
static const int BT_ON_MS   = 400;    // 亮 400ms
static const int BT_GAP_MS  = 400;    // 灭 400ms

static void btPulse(int ms)
{
    btLed(true);
    delay(ms);
    btLed(false);
    delay(ms);
}

// 报出"级数": n 个脉冲 + 一段长灭(便于分辨轮与轮之间的边界)。
static void btReport(int n)
{
    for (int i = 0; i < n; i++) {
        btPulse(400);
    }
    delay(2000);                       // 轮间停顿: 明显长于脉冲内部间隔
}

// ============================================================
// 闪码格式(第二版: 全部靠"数", 不靠"分辨长短")
// ============================================================
// 上一版用"超长闪 1.2s 作起始标记 + 长闪 0.6s 报数"来区分两段 —— 实测反馈
// 是两者肉眼看不出区别(1.2s 和 0.6s 确实太接近), 导致读不出来。
//
// 现在改成: 【所有亮的时间完全一样(300ms)】, 只用【灭的时间】区分含义。
// 人眼对"停顿明显久一点"的判别, 远比"亮得久一点"可靠。
//
// 一轮完整的闪码:
//
//   ① 亮 300ms, 灭 300ms  ── 重复 3 次 ──   "三短" = 一段开始
//   ② 灭 1200ms                              (很长, 明显)
//   ③ 亮 300ms, 灭 300ms  ── 重复 S 次 ──   〖阶段码 S〗
//   ④ 灭 1200ms                              (很长, 明显)
//   ⑤ 亮 300ms, 灭 300ms  ── 重复 E 次 ──   〖错误码低 3 位 = E〗
//   ⑥ 灭 3000ms                              (超长, 一轮结束)
//
// 读法: 听到/看到"先来三下"就知道这一轮开始了, 然后数中间那段几下(阶段),
//       再数最后那段几下(错误码)。E 段可能为 0 次(错误码低 3 位为 0),
//       此时会直接跳到 ⑥ 的超长停, 表现为"数完阶段码之后就长时间不亮"。
//
// 举例: 三短 / 4 下 / 2 下  =>  阶段码 4, 错误码低 3 位 = 2 (0x102 参数非法)
//       三短 / 5 下 / 0 下  =>  阶段码 5(成功), 没有错误码段
// ============================================================

static const int BT_PULSE_MS  = 300;   // 亮 300ms —— 所有脉冲统一
static const int BT_SHORT_GAP = 300;   // 脉冲之间: 300ms
static const int BT_LONG_GAP  = 1200;  // 分段之间: 1200ms(明显长于 300ms)
static const int BT_ROUND_GAP = 3000;  // 轮与轮之间: 3s

// 发 n 个脉冲, 每个 300ms 亮 + 300ms 灭
static void btBurst(int n)
{
    for (int i = 0; i < n; i++) {
        btLed(true);  delay(BT_PULSE_MS);
        btLed(false); delay(BT_SHORT_GAP);
    }
}

// 完整一轮闪码: 三短(起始) → 阶段码 → 错误码(可选)
//
// stageErr < 0 表示"没有错误码段"(成功或未失败时用), 此时省略第 ⑤ 段,
// 避免用"0 下"这种容易和"我没数清"混淆的表达。
static void btRound(int stage, int stageErr)
{
    btBurst(3);              // ① 三短: 一轮开始
    delay(BT_LONG_GAP);      // ②

    btBurst(stage);          // ③ 阶段码
    delay(BT_LONG_GAP);      // ④

    if (stageErr >= 0) {
        btBurst(stageErr);   // ⑤ 错误码低 3 位
    }

    delay(BT_ROUND_GAP);     // ⑥ 一轮结束
}

void bootTestStart()
{
    pinMode(BT_LED, OUTPUT);

    // 阶段 1: 3 次长闪 —— 证明 setup() 已进入
    for (int i = 0; i < 3; i++) {
        btLed(true);
        delay(600);
        btLed(false);
        delay(400);
    }

    // 阶段 2: 持续快闪 —— 证明 setup() 跑完, 启动链路完好
    for (;;) {
        btLed(true);
        delay(100);
        btLed(false);
        delay(100);
    }
}

// ------------------------------------------------------------
// USB 分级诊断
// ------------------------------------------------------------
// 注意: 本构建【不】调用 usbHost.begin(), 而是手工逐级重做它内部的步骤,
// 以便在每一级之后能把结果编码成闪码。这样做的代价是与 begin() 的实现
// 有重复, 但它是一次性排查工具, 换来的是"无需串口即可定位"。
void usbStageStart()
{
    pinMode(BT_LED, OUTPUT);
    btLed(false);
    delay(500);

    // ---- 级 1: usb_host_install ----
    // 这一级失败通常意味着 S3 唯一的 USB PHY 没拿到(被 USB-Serial/JTAG 占用,
    // 或 efuse USB_PHY_SEL 指向了 Device 侧)。
    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        for (;;) btReport(1);            // 级 1 失败
    }

    // ---- 级 2: 注册 client ----
    //
    // ★ 上一版这里失败(实测 2 个脉冲), 原因已从反汇编确认:
    //   usb_host_client_register() 内部做如下校验(见 libusb.a 的 usb_host.c.obj):
    //       l8ui  a4, a2, 0     ; config->is_synchronous   (结构体偏移 0)
    //       bnez  a4, skip      ; is_synchronous != 0 就跳过回调检查
    //       l32i  a4, a2, 8     ; config->async.client_event_callback (偏移 8)
    //       beqz  a4, fail      ; 回调为 NULL -> 返回 ESP_ERR_INVALID_ARG(0x102)
    //   也就是说: 异步客户端(is_synchronous=false)【必须】提供事件回调。
    //   上一版为了"简化"传了 nullptr, 反而触发了这条合法性检查 —— 那是本诊断
    //   程序自己的 bug, 不是生产固件的问题。
    //
    //   注意这同时反证了一件事: 级 1 (usb_host_install) 是【成功】的 ——
    //   能走到级 2 才失败, 说明 USB PHY 已正常拿到, 硬件侧的怀疑可以排除。
    //
    //   本版挂一个真正的事件回调: 由它在 NEW_DEV 回调内部完成 device_open。
    const usb_host_client_config_t client_config = {
        .is_synchronous    = false,
        .max_num_event_msg = 10,
        .async = {
            .client_event_callback = usbStageEventCb,
            // 回调不再依赖 callback_arg: 句柄走文件级静态变量 g_client,
            // 避免传局部变量地址产生的野指针(见 g_client 处的说明)。
            .callback_arg          = nullptr,
        }
    };
    err = usb_host_client_register(&client_config, &g_client);
    if (err != ESP_OK || g_client == NULL) {
        for (;;) btReport(2);            // 级 2 失败
    }

    // ---- 级 3 / 4 / 5: 驱动事件循环, 由回调完成设备打开 ----
    //
    // ★ 演进记录(每一步都是本诊断自身的缺陷, 逐版修正):
    //   第 1 版 报 2: 回调传 nullptr -> 被 client_register 的合法性检查拒绝。
    //   第 2 版 报 3: 补了回调, 但从未运行事件循环 -> 枚举根本不会推进。
    //   第 3 版 报 4: 补了事件循环, 却在【回调外面】轮询着调用 device_open ——
    //                设备只在 NEW_DEV 派发的那一刻可认领, 出了那一刻就 open 不了。
    //
    //   本版交给 usbStageEventCb 在 NEW_DEV 回调内部完成 open, 与生产固件
    //   (_clientEventCallback) 的做法一致。设备打开结果记在 g_stage 里。
    //
    //   循环职责只剩两件: 持续驱动两条事件通道, 然后把 g_stage 报出来。
    for (;;)
    {
        // 1) 库级事件(设备 attach/detach、枚举推进)
        uint32_t libFlags = 0;
        usb_host_lib_handle_events(0, &libFlags);

        // 2) 客户端级事件 —— NEW_DEV 会在这里被派发进 usbStageEventCb
        usb_host_client_handle_events(g_client, 0);

        // 让出 CPU: 忙等会饿死 IDLE 任务并触发看门狗复位,
        // 那样灯会重启闪(看起来像"另一次启动"), 结论就被污染了。
        vTaskDelay(pdMS_TO_TICKS(20));

        // 报出当前阶段。g_stage 默认 3(还没枚举到), 回调成功则升到 5。
        //
        // 新格式: 三短(起始) / 阶段码 / [错误码] , 全部靠数脉冲个数读,
        // 不再依赖分辨闪的长短(上一版那样读不出来, 见 btRound 上方说明)。
        //   阶段码 3 = 还没枚举到
        //   阶段码 4 = 枚举到但 device_open 失败, 后面跟错误码低 3 位
        //   阶段码 5 = 枚举完全成功, 没有错误码段(直接长时间不亮)
        //   阶段码 6 = open 成功但取配置描述符失败, 后面跟错误码低 3 位
        const int stage = g_stage;
        if (stage == 4 || stage == 6) {
            btRound(stage, g_err & 0x7);   // 带错误码
        } else if (stage == 5) {
            btRound(5, -1);                // 成功: 无错误码段
        } else {
            btRound(3, -1);                // 还没枚举到: 无错误码段
        }
    }
}
