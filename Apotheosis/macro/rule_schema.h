#pragma once
#include "macro_config.h"

namespace macros {
struct Choice { const char* id; const char* label; };
inline const std::vector<Choice>& events() {
    static const std::vector<Choice> v={
        {"key_down",u8"按键按下"},{"key_up",u8"按键松开"},{"key_held",u8"按键按住"},
        {"click",u8"单击"},{"double",u8"双击"},{"triple",u8"三击"},{"long",u8"长按"},{"short",u8"短按"},
        {"sequence",u8"按键序列"},{"target_found",u8"获得检测目标"},{"target_lost",u8"失去检测目标"},
        {"fov_enter",u8"目标进入 FOV"},{"fov_leave",u8"目标离开 FOV"},
        {"target_switch",u8"切换目标"},{"target_lock",u8"锁定目标"},{"target_unlock",u8"解锁目标"},
        {"target_first",u8"首次获得目标"},{"target_reacquire",u8"丢失后重新获得目标"},{"target_held",u8"持续存在目标"},
        {"aim_start",u8"自瞄开始"},{"aim_stop",u8"自瞄停止"},{"trigger_start",u8"扳机开始"},{"trigger_stop",u8"扳机停止"},
        {"conditions",u8"条件满足"},{"color_found",u8"找色命中"},{"color_lost",u8"找色丢失"},
        {"image_found",u8"找图命中"},{"image_lost",u8"找图丢失"},{"ocr_found",u8"OCR 命中"},{"ocr_lost",u8"OCR 丢失"},
        {"metric_change",u8"数值 / 数量 / 类别 / 置信度 / 变量变化"},
        {"timer",u8"计时器到达"},{"cooldown",u8"冷却结束"},{"rule_enable",u8"规则启用"},{"rule_disable",u8"规则禁用"},
        {"profile_change",u8"配置切换"},{"window_enter",u8"窗口获得焦点"},{"window_leave",u8"窗口失去焦点"},
        {"process_found",u8"指定进程出现"},{"window_found",u8"指定窗口出现"},{"resolution_change",u8"分辨率变化"}};
    return v;
}
inline const std::vector<Choice>& metrics() {
    static const std::vector<Choice> v={
        {"all",u8"条件组：全部满足 AND"},{"any",u8"条件组：任意满足 OR"},{"none",u8"条件组：均不满足 NOT"},
        {"xor",u8"条件组：异或 XOR（奇数项满足）"},{"at_least",u8"条件组：满足数量 ≥ 阈值"},
        {"target.exists",u8"存在目标"},{"target.count",u8"检测数量"},{"target.class",u8"目标类别"},
        {"target.confidence",u8"目标最大置信度 0～1"},{"aim.active",u8"自瞄激活"},{"trigger.active",u8"扳机激活"},
        {"aim.key",u8"自瞄键按下"},{"color.hit",u8"准星找色命中"},{"target.max_size",u8"指定类别最大宽度及高度 px"},
        {"target.max_height",u8"指定类别最大高度 px"},{"target.max_width",u8"指定类别最大宽度 px"},
        {"key",u8"指定按键 / 组合键按下"},{"target.min_height",u8"指定类别最小高度 px"},
        {"target.min_width",u8"指定类别最小宽度 px"},{"target.mean_height",u8"指定类别平均高度 px"},
        {"target.mean_width",u8"指定类别平均宽度 px"},{"target.area",u8"目标最大面积 px²"},
        {"target.ratio",u8"目标宽高比"},{"target.distance",u8"目标屏幕距离 px"},{"target.angle",u8"目标屏幕方向角 °"},
        {"target.x",u8"目标中心 X"},{"target.y",u8"目标中心 Y"},{"target.fov",u8"目标在 FOV 内"},
        {"target.region",u8"目标在指定区域"},{"target.speed",u8"目标画面速度 px/s"},
        {"target.direction",u8"目标画面运动方向 °"},{"target.lock_ms",u8"目标锁定时长 ms"},{"target.id",u8"目标 ID"},
        {"target.visible",u8"目标当前可见"},{"target.occluded",u8"原选中目标暂时未检出"},{"target.locked",u8"控制器已锁定目标"},
        {"image.hit",u8"模板找图命中"},{"image.score",u8"模板匹配相似度 0～1"},
        {"pixels.count",u8"区域内颜色数量"},{"pixels.multi",u8"多点找色命中"},{"pixel.value",u8"指定像素 RGB 数值"},
        {"image.change",u8"画面变化比例 0～1"},{"ocr.text",u8"OCR 文本"},
        {"mouse.x",u8"本机鼠标 X"},{"mouse.y",u8"本机鼠标 Y"},{"pad.axis",u8"手柄摇杆 -1～1 / 扳机 0～1"},
        {"window.title",u8"前台窗口标题"},{"window.exists",u8"指定窗口存在"},{"process.name",u8"前台进程名"},
        {"process.exists",u8"指定进程存在"},{"clock.minute",u8"当天分钟 0～1439"},
        {"cooldown.ready",u8"冷却结束"},{"loop.index",u8"循环索引"},{"variable",u8"变量值"},{"probability",u8"随机概率 %"}};
    return v;
}
struct Setting { const char* id; const char* label; const char* initial; const char* help; };
inline const std::vector<Setting>& settings() {
    static const std::vector<Setting> v={
        {"description",u8"说明","",u8"自由备注"},{"group",u8"分组","",u8"用于整理宏"},
        {"priority",u8"优先级","0",u8"数值越大越先处理"},{"mutex",u8"互斥组","",u8"相同非空组不能同时运行"},
        {"parallel",u8"允许与其他宏并行","0",u8"0 关闭 / 1 开启；共享输入有独立持有计数"},
        {"preempt",u8"允许抢占低优先级宏","0",u8"0 关闭 / 1 开启；先释放被中断宏的输入"},
        {"level",u8"条件持续满足时重复触发","0",u8"0 仅假→真 / 1 持续触发；遵守重入锁和冷却"},
        {"debounce_ms",u8"条件防抖 ms","0",u8"条件需持续满足这么久"},
        {"cooldown_ms",u8"触发冷却 ms","0",u8"从成功启动起计算"},
        {"delay_min_ms",u8"启动延迟最短 ms","0",u8"延迟后执行第一步"},{"delay_max_ms",u8"启动延迟最长 ms","0",u8"大于最短值时随机延迟"},
        {"max_count",u8"最大启动次数","0",u8"0 不限；切换配置后重置"},
        {"window_ms",u8"限流时间窗 ms","1000",u8"配合窗口内最多次数"},{"window_count",u8"窗口内最多次数","0",u8"0 不限"},
        {"target_lock",u8"同一目标只触发一次","0",u8"目标丢失或切换后可重新触发"},
        {"key_lock",u8"按住期间只触发一次","0",u8"触发组合键全部松开后解锁"},
        {"cancel_conditions",u8"条件失效时取消","1",u8"0 仅启动时检查 / 1 运行中持续检查"},
        {"cancel_target",u8"目标丢失或切换时取消","0",u8"针对启动时的选中目标"},
        {"cancel_key",u8"触发键松开时取消","0",u8"事件宏也可绑定取消键"},
        {"timeout_ms",u8"最长运行时长 ms","60000",u8"0 不限；停止键始终有效"},
        {"gesture_ms",u8"长按 / 多击 / 序列间隔 ms","350",u8"长按阈值或连续输入允许间隔"},
        {"timer_ms",u8"计时器周期 ms","1000",u8"启用后周期触发"},
        {"event_metric",u8"变化事件监测量","target.count",u8"条件列表中的指标 ID；变量写 variable:名称"},
        {"pattern",u8"事件匹配窗口 / 进程 / OCR 文本","",u8"窗口标题支持包含；进程名精确匹配"},
        {"scope_window",u8"生效前台窗口标题","",u8"留空不限；按包含匹配"},
        {"scope_process",u8"生效前台进程名","",u8"留空不限，例如 notepad.exe"},
        {"scope_width",u8"生效识别宽度","0",u8"0 不限"},{"scope_height",u8"生效识别高度","0",u8"0 不限"},
        {"cancel_condition",u8"额外中断条件编号","0",u8"0 无；某个条件行满足时停止"},
        {"vision_interval_ms",u8"找图 / OCR 检查间隔 ms","200",u8"后台处理，不阻塞控制线程"}};
    return v;
}
inline bool groupMetric(const std::string& s) { return s=="all"||s=="any"||s=="none"||s=="xor"||s=="at_least"; }
inline std::string label(const std::vector<Choice>& choices,const std::string& id) {
    for(const auto& c:choices)if(id==c.id)return c.label;return id;
}
inline const std::vector<const char*>& actionLabels() {
    static const std::vector<const char*> v={u8"等待 / 随机等待",u8"键盘按下",u8"键盘松开",u8"键盘点按",u8"鼠标相对移动",
        u8"鼠标按下",u8"鼠标松开",u8"鼠标点击",u8"滚轮",u8"重置 PID XY",u8"重置 PID X",u8"重置 PID Y",
        u8"次数循环开始",u8"循环结束",u8"条件循环开始",u8"如果条件满足",u8"否则",u8"分支结束",u8"跳出循环",u8"继续下轮",
        u8"等待条件 / 超时",u8"停止本宏",u8"设置变量",u8"变量加减",u8"记录日志",u8"调用其他宏",u8"启用规则",u8"禁用规则",
        u8"暂停规则",u8"恢复规则",u8"输入文本",u8"鼠标绝对移动",u8"平滑移动",u8"曲线移动",u8"移动到目标偏移点",u8"移动到预测点",
        u8"锁定当前目标",u8"解除目标锁定",u8"切换下个目标",u8"清除目标",u8"设置瞄准部位",u8"选择目标类别",u8"设置选择优先级",
        u8"设置预测",u8"设置平滑系数",u8"设置速度限制",u8"播放声音",u8"手柄震动",u8"通知提示",u8"切换配置方案",
        u8"导出配置",u8"导入配置",u8"并行分支开始",u8"并行分支结束",u8"条件重试循环",u8"遍历目标循环",u8"按变量分支",u8"分支值",
        u8"多路分支结束",u8"条件跳转"};return v;
}
struct ActionFields {const char *a="",*b="",*c="",*d="",*value="",*text="",*help="";};
inline ActionFields actionFields(ActionType t) {
    switch(t){
    case ActionType::Loop:return {u8"循环次数"};
    case ActionType::While:case ActionType::If:return {u8"条件编号（0=全部启动条件）"};
    case ActionType::WaitCondition:return {u8"条件编号",u8"超时 ms"};
    case ActionType::Retry:return {u8"成功条件编号",u8"最多尝试次数",u8"重试间隔 ms"};
    case ActionType::ForTargets:return {"","",u8"类别 ID（-1=全部）"};
    case ActionType::Jump:return {u8"跳到步骤编号",u8"条件编号（0=全部）"};
    case ActionType::SetVariable:return {"","","","",u8"数值",u8"变量名"};
    case ActionType::AddVariable:return {"","",u8"0 加 / 1 减 / 2 乘 / 3 除 / 4 最小 / 5 最大","",u8"数值",u8"变量名"};
    case ActionType::Switch:return {"","","","","",u8"变量名"};
    case ActionType::Case:return {"","","","",u8"匹配数值",u8"默认分支填 default，否则留空"};
    case ActionType::Call:return {u8"0 等待返回 / 1 并行调用","","","","",u8"宏名称或 ID"};
    case ActionType::EnableRule:case ActionType::DisableRule:case ActionType::PauseRule:case ActionType::ResumeRule:return {"","","","","",u8"规则名称或 ID"};
    case ActionType::Text:return {"","","","","",u8"文本（Windows 原生输出）"};
    case ActionType::AbsoluteMove:return {u8"桌面 X 像素",u8"桌面 Y 像素","","","","",u8"本机虚拟桌面的绝对位置，需要 Windows 原生输出。"};
    case ActionType::SmoothMove:case ActionType::CurveMove:return {u8"X 设备计数",u8"Y 设备计数",u8"移动时长 ms",u8"曲线垂直弯曲量（计数）"};
    case ActionType::MoveToTarget:case ActionType::MoveToPrediction:return {u8"目标 X 偏移 px",u8"目标 Y 偏移 px",u8"预测时长 ms","",u8"每像素对应设备计数","",u8"以本帧目标和准星计算一次位移；换算值应与设备灵敏度匹配。"};
    case ActionType::AimPart:return {u8"框内 X %（左 0，右 100）",u8"框内 Y %（上 0，下 100）"};
    case ActionType::AimClass:return {u8"类别 ID（-1 恢复配置）"};
    case ActionType::AimPriority:return {u8"0 配置 / 1 最近 / 2 最远 / 3 置信度 / 4 最大框"};
    case ActionType::Prediction:return {"","","","",u8"额外预测时长 ms（0 关闭）"};
    case ActionType::Smoothing:return {"","","","",u8"平滑系数 0.01～1（1 关闭）"};
    case ActionType::SpeedLimit:return {"","","","",u8"每秒设备计数上限（0 不限）"};
    case ActionType::Vibration:return {u8"左电机 %",u8"右电机 %",u8"手柄编号 0～3",u8"持续 ms"};
    case ActionType::Sound:return {"","","","","",u8"WAV 文件路径（留空系统提示音）"};
    case ActionType::Notification:case ActionType::Log:return {"","","","","",u8"提示 / 日志内容"};
    case ActionType::SwitchProfile:return {"","","","","",u8"配置方案名称"};
    case ActionType::ExportConfig:case ActionType::ImportConfig:return {"","","","","",u8"INI 文件完整路径","系统操作交给界面线程执行，结果显示在状态栏。"};
    default:return {};
    }
}
}
