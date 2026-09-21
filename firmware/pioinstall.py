"""
pioinstall.py — 在 DSH 沙箱下绕过 ".github 无法创建" 的包安装助手。

背景:
  PlatformIO 解包任何 registry 包时都会原样还原 .github/ 目录, 而本沙箱禁止
  在 workspace 内创建该名称的路径 (WinError 5)。于是 `pio pkg install` 永远
  失败在解包阶段 —— 但 tarball 其实已经完整下载到了 .cache/downloads/。

做法:
  1) 调用一次 PlatformIO 让它把 tarball 下载进 .cache/downloads (必然失败, 无所谓);
  2) 自己用 tarfile 解包, 跳过 .github 条目;
  3) 按 PlatformIO 的目录约定放到 piocore/packages/<name> 或 piocore/platforms/<name>。

包里 .github 只是 CI 配置, 对编译毫无影响, 跳过完全安全。
"""
import json
import os
import re
import sqlite3
import sys
import tarfile
import zipfile

CORE = os.path.abspath("piocore")
DL = os.path.join(CORE, ".cache", "downloads")
PKGS = os.path.join(CORE, "packages")
PLATS = os.path.join(CORE, "platforms")


def skip(name: str) -> bool:
    return ".github" in name.replace("\\", "/").split("/")


def extract(archive: str, dest: str) -> int:
    os.makedirs(dest, exist_ok=True)
    n = 0
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            for m in z.namelist():
                if skip(m):
                    continue
                z.extract(m, dest)
                n += 1
    else:
        with tarfile.open(archive) as t:
            for m in t.getmembers():
                if skip(m.name):
                    continue
                try:
                    t.extract(m, dest, filter="data")
                    n += 1
                except Exception as e:  # noqa: BLE001
                    print(f"    skip {m.name}: {e}")
    return n


def main() -> None:
    os.makedirs(PKGS, exist_ok=True)
    os.makedirs(PLATS, exist_ok=True)

    if not os.path.isdir(DL):
        print(f"no download cache at {DL}")
        return

    files = [
        os.path.join(DL, f)
        for f in os.listdir(DL)
        if os.path.isfile(os.path.join(DL, f)) and os.path.getsize(os.path.join(DL, f)) > 4096
    ]
    print(f"found {len(files)} cached archive(s)")

    for f in files:
        # 先看压缩包里的根目录/package.json, 判断包名与类型
        name = kind = None
        try:
            if zipfile.is_zipfile(f):
                with zipfile.ZipFile(f) as z:
                    names = z.namelist()
                    data = z.read("package.json") if "package.json" in names else None
            else:
                with tarfile.open(f) as t:
                    names = t.getnames()
                    try:
                        data = t.extractfile("package.json").read()
                    except Exception:  # noqa: BLE001
                        data = None
            if data:
                meta = json.loads(data)
                name = meta.get("name")
                kind = meta.get("type")  # "platform" 或 "tool"/"framework"
        except Exception as e:  # noqa: BLE001
            print(f"  {os.path.basename(f)}: cannot read manifest ({e})")

        if not name:
            # 退回到用根目录名猜
            root = names[0].split("/")[0] if names else os.path.basename(f)
            name, kind = root, "tool"

        base = PLATS if kind == "platform" else PKGS
        dest = os.path.join(base, name)

        if os.path.isdir(dest) and os.listdir(dest):
            print(f"  {name:<40} already present, skip")
            continue

        n = extract(f, dest)
        print(f"  {name:<40} -> {os.path.relpath(dest, CORE):<45} {n} entries")

    print("done")


if __name__ == "__main__":
    main()
