import sys, time, glob

try:
    import serial
except ImportError:
    print("pyserial 未安装, 尝试安装...")
    import subprocess
    subprocess.run([sys.executable, "-m", "pip", "install", "pyserial", "-q"], check=False)
    import serial

def dump(port, seconds=6, baud=115200):
    print("=" * 60)
    print("读取 %s @ %d ..." % (port, baud))
    print("=" * 60)
    try:
        s = serial.Serial(port, baud, timeout=0.3)
    except Exception as e:
        print("打开失败: %s" % e)
        return
    t0 = time.time()
    buf = b""
    while time.time() - t0 < seconds:
        try:
            d = s.read(4096)
            if d:
                buf += d
        except Exception as e:
            print("读取出错: %s" % e)
            break
    s.close()

    if not buf:
        print("(没有收到任何数据)")
        return
    txt = buf.decode("utf-8", errors="replace")
    print("收到 %d 字节:" % len(buf))
    print("-" * 60)
    print(txt)
    print("-" * 60)

for p in ["COM5", "COM3"]:
    dump(p, 6)
    print()
