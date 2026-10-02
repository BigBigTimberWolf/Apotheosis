"""Reproduce log summaries and the bounded, offline audit report."""
import csv, hashlib, html, json, pathlib, re, statistics

here = pathlib.Path(__file__).resolve().parent
root = here.parent.parent
summary = []
for p in sorted((root / 'build/cuda/Release/logs').glob('latency_2026-09-*.log')):
    vals = []
    for line in p.read_text(encoding='utf-8-sig', errors='replace').splitlines():
        if ' | E2E=' not in line:
            continue
        d = {k: float(v) for k,v in re.findall(r'([A-Za-z0-9_]+)=(-?\d+(?:\.\d+)?)', line)}
        if d.get('E2E', -1) > 0 and d.get('aim2mv', 0) > 0:
            vals.append(d)
    if vals:
        summary.append(dict(file=p.name, summary_lines=len(vals),
            median_summary_pub2aim_ms=statistics.median(x['pub2aim'] for x in vals),
            median_summary_aim2mv_ms=statistics.median(x['aim2mv'] for x in vals),
            max_summary_aim2mv_ms=max(x['aim2mv'] for x in vals)))
(here/'log_summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
rows = list(csv.DictReader((here/'probe_results.csv').open(encoding='utf-8-sig')))
selected = [r for r in rows if r['kind']=='production_controller'
            and r['gain_px_per_count']=='0.50' and r['feedback_delay_ms']=='0']
manifest = {}
for p in [*sorted((root/'Apotheosis/control').glob('*.*')),
          root/'Apotheosis/runtime/aim_loop.cpp', root/'Apotheosis/runtime/motion_feedback_window.h',
          root/'Apotheosis/mouse/latest_move_slot.h',root/'build/cuda/Release/configs/洲.ini',
          here/'flow_probe.cpp']:
    manifest[str(p.relative_to(root))] = hashlib.sha256(p.read_bytes()).hexdigest()
(here/'source_hashes.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False),encoding='utf-8')
table = ''.join('<tr>'+''.join('<td>'+html.escape(str(v))+'</td>' for v in [
    r['profile'],r['hz'],r['first_move_ms'],r['first_within_2px_ms'],r['settled_100ms_start_ms'],r['reversals']])+'</tr>' for r in selected)
logtable = ''.join('<tr>'+''.join('<td>'+html.escape(str(v))+'</td>' for v in d.values())+'</tr>' for d in summary[-2:])
doc = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>AM 与 Apotheosis：流程复查和离线测试</title>
<style>body{font:16px/1.7 system-ui;max-width:1100px;margin:40px auto;padding:0 24px;color:#202b38}table{border-collapse:collapse;width:100%;font-size:14px}td,th{padding:9px;border:1px solid #ccd3dc;text-align:left}h1{font-size:26px}.note{background:#fff3d3;padding:15px}code{background:#eee}a{color:#1667a8}</style>
<h1>AM 与 Apotheosis：流程复查和离线测试</h1>
<p class="note"><b>结论范围：</b>已完成当前源码复查、历史日志统计和 138 个离线场景。没有运行 AM 实战对照，不能声称已证明 AM 更快的唯一原因。没有修改产品控制代码。</p>
<h2>可以确认的事实</h2>
<ol><li><b>启动确实有一次初始化返回。</b>aim_loop.cpp 的 g_first_tick 分支设置时钟后直接返回；启动/重置后要等下一次处理机会。正常持续瞄准不重复这一步。延迟计时在该分支之后开始，因此低延迟日志不能排除这一步。此前“没有固定多等一次”的表述不完整，现已纠正。</li>
<li><b>正常处理阶段没有日志证据显示长期排队。</b>最近两份有输出的日志，周期摘要的 pub2aim 中位数 0.01 ms，aim2mv 中位数 0.19 / 0.20 ms。它们是平滑统计值的中位数，不是每次移动延迟的分位数，不保证最坏情况，也不证明日志对应洲.ini。</li>
<li><b>Apotheosis 普通瞄准按检测发布调用。</b>鼠标线程发送已经计算的相对移动。AM 的 FrameSync/Kalman 分支存在按经过毫秒积分输出；EventSync 按新数据工作。高频输出本身不证明更快，AM 实际模式未知。</li>
<li><b>补偿确实可能让“首次接近”与“最终停稳”分离。</b>下表是生产控制器中的跟随补偿开/关消融，其余输入相同。不能将某个模拟参数下的数字当作用户游戏成绩。</li>
<li><b>慢发送条件下，待发送移动会被新移动覆盖。</b>真实 LatestMoveSlot 测试：生成 120 次各 10 counts，消费者每 1/2/4 次取一次，分别取到 1200/600/300 counts。这是丢弃旧指令以保持新鲜的既有设计，不能据此断言用户设备实际吞吐不足。</li></ol>
<h2>闭环模拟：使用当前生产控制代码</h2>
<p>水平静止目标初始误差 80 px；画面 416×416；每个鼠标 count 假设移动 0.5 px；本表额外反馈延迟 0。读取真实 tracker、anchor、follow compensator、PID 和 MotionFeedbackWindow；主瞄准 P=.4，I=.01，D=.01，FF=.001，限幅416，分段3，跟随10，autoFire=true。初始化返回在测试壳中按源码模拟。关闭补偿对照仅将 follow 设为0。</p>
<p><b>不包含：</b>真实推理、找色、操作系统线程、路径整形、USB/游戏反馈、开镜与副瞄准切换。真实灵敏度未标定；输入是合成静止框。这不是完整实战回放，更不是 AM 仿真。</p>
<p>最终停稳：进入 ±2 px 后直到 3秒测试结束均不再离开，且至少维持100 ms；-1表示未满足。反向次数包括很小的修正，不能等同于肉眼抖动。0 ms“首次动作”只适用于独立 PID 子测试，不是计算耗时。</p>
<table><tr><th>配置</th><th>结果频率 Hz</th><th>首步 ms</th><th>首次 ±2px ms</th><th>最终停稳 ms</th><th>反向次数</th></tr>'''+table+'''</table>
<h2>最近两份有输出日志的摘要</h2><table><tr><th>文件</th><th>有效摘要行</th><th>发布→消费中位数 ms</th><th>消费→发送中位数 ms</th><th>摘要最大值 ms（非单次最坏延迟）</th></tr>'''+logtable+'''</table>
<h2>AM 二次核对的证据</h2><ul>
<li>FUN_14007e5c0：同一画面对象传给找色和推理；调用0824e0后再调用088130。</li>
<li>FUN_1400824e0：更新控制数据序号870行，通知953行；动态准星换算约489—514行，扳机使用740行，控制误差发布910行。</li>
<li>FUN_140067000：事件/定时等待约270—335行，模式选择约530—605行；新数据判断1124行附近；FrameSync/Kalman在1500—1610行附近按时间乘控制量后取整。</li>
<li>二进制常量核验：0x1401f5ce0=1000000，0x1401f80f8=10。上述输出间隔为毫秒且截断到[0,10]，并不是每次循环无条件发送一整份位移。</li>
<li>FUN_1400774e0 / FUN_140067000：按键请求计数和移动等待，证明按键优先；不证明具体省时。</li>
<li>归档仅有反编译及分析材料，未找到 AM 使用者的运行配置或逐帧控制实测记录。</li></ul>
<h2>验证与复现</h2><p>3项现有回归通过：recovered_controller_test、aimpoint_prediction_test、motion_feedback_window_test。138个场景包含81个PID子场景、54个控制器场景、3个发送槽场景。场景数不等于138项实战性能断言。</p>
<p>最初尝试构建 latest_move_slot_test 时，该目标未注册，构建报错；随后将真实槽类的验证纳入独立 flow_probe，3个槽场景断言通过。未掩盖失败或使用旧目标结果。</p>
<p>复现：CMake配置本目录，Release构建flow_probe；运行输出保存probe_results.csv，再运行summarize.py。source_hashes.json记录本次源码版本。</p>
<p><a href="probe_results.csv">全部原始结果</a> · <a href="flow_probe.cpp">测试源代码</a> · <a href="log_summary.json">日志统计</a> · <a href="regression_results.txt">回归测试输出</a></p>
<h2>仍未被证明的结论</h2><p>不能证明 AM 的优势由单一模式、质心、Kalman 或线程频率造成；缺少同一输入下 AM 的配置、准星轨迹及实际位移记录，也缺少用户游戏的鼠标到画面比例。当前证据能够定位可复现问题和纠正错误解释，不能替代这场对照实验。</p></html>'''
(here/'report.html').write_text(doc,encoding='utf-8')
print('Wrote report.html; scenarios:',len(rows),'log files with valid output:',len(summary))
