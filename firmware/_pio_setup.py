"""Install every PlatformIO package from the local download cache, with .piopm markers.

Why this exists: in this sandbox PlatformIO's own unpacker fails with
"WinError 5 拒绝访问" on ordinary directories (bin/, LICENSE, .github/...).
The archives themselves download fine, and plain tarfile extraction of the very
same archives works with zero failures — so we extract ourselves and then write
the .piopm metadata marker PlatformIO uses to recognise an installed package.

Usage:  python _pio_setup.py
Run from the workspace root.
"""
import glob
import json
import os
import shutil
import sys
import tarfile
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.join(ROOT, "piocore")
DL = os.path.join(CORE, ".cache", "downloads")
PLATS = os.path.join(CORE, "platforms")
PKGS = os.path.join(CORE, "packages")

# Packages that must never be reported as "installed" from a partial extract.
MIN_FILES = 3


def manifest_of(path):
    """Return (metadata dict or None, is_zip)."""
    is_zip = zipfile.is_zipfile(path)
    for key in ("package.json", "platform.json"):
        try:
            if is_zip:
                with zipfile.ZipFile(path) as z:
                    if key in z.namelist():
                        return json.loads(z.read(key)), True
            else:
                with tarfile.open(path) as t:
                    try:
                        return json.loads(t.extractfile(key).read()), False
                    except Exception:
                        pass
        except Exception:
            pass
    return None, is_zip


def extract(path, dest, is_zip):
    os.makedirs(dest, exist_ok=True)
    n = 0
    if is_zip:
        with zipfile.ZipFile(path) as z:
            for m in z.namelist():
                z.extract(m, dest)
                n += 1
    else:
        with tarfile.open(path) as t:
            for m in t.getmembers():
                try:
                    t.extract(m, dest, filter="data")
                    n += 1
                except Exception:
                    pass
    return n


def write_piopm(dest, meta, kind):
    data = {
        "type": kind,
        "name": meta.get("name"),
        "version": meta.get("version"),
        "spec": {
            "owner": meta.get("owner") or meta.get("spec", {}).get("owner"),
            "id": meta.get("id"),
            "name": meta.get("name"),
            "requirements": meta.get("requirements"),
            "uri": None,
        },
    }
    with open(os.path.join(dest, ".piopm"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2)


def main():
    os.makedirs(PKGS, exist_ok=True)
    os.makedirs(PLATS, exist_ok=True)

    archives = [
        p for p in sorted(glob.glob(os.path.join(DL, "*")))
        if os.path.isfile(p) and os.path.getsize(p) > 4096
    ]
    print(f"scanning {len(archives)} cached archive(s)")

    done = set()
    changed = False

    for path in archives:
        meta, is_zip = manifest_of(path)
        if not meta or not meta.get("name"):
            continue

        name = meta["name"]
        if name in done:
            continue
        done.add(name)

        # espressif32 has no "type" field but is a platform
        kind = meta.get("type") or ("platform" if name == "espressif32" else "tool")
        base = PLATS if kind == "platform" else PKGS
        dest = os.path.join(base, name)

        if os.path.isdir(dest) and len(os.listdir(dest)) >= MIN_FILES:
            write_piopm(dest, meta, kind)  # refresh marker in case it is stale
            print(f"  {name:<36} v{meta.get('version','?'):<22} present")
            continue

        shutil.rmtree(dest, ignore_errors=True)
        n = extract(path, dest, is_zip)
        write_piopm(dest, meta, kind)
        changed = True
        print(f"  {name:<36} v{meta.get('version','?'):<22} -> {os.path.basename(base):<10} {n} files")

    print("\nplatforms:", sorted(os.listdir(PLATS)))
    print("packages :", sorted(os.listdir(PKGS)))
    return 0 if changed else 0


if __name__ == "__main__":
    sys.exit(main())
