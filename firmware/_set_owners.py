"""Set the owner field on all .piopm markers so owner-qualified lookups resolve.

PlatformIO resolves `espressif/toolchain-xtensa-esp32s3` by owner+name; with
owner=None the lookup returns None and the package is reinstalled (which then
fails in this sandbox). The owner comes from the platform's package manifest.
"""
import io, json, os

# owner for each package, as declared in espressif32's platform.json
OWNERS = {
    "toolchain-xtensa-esp32s3": "espressif",
    "toolchain-riscv32-esp": "espressif",
    "toolchain-xtensa-esp32": "espressif",
    "toolchain-xtensa-esp32s2": "espressif",
    "framework-arduinoespressif32": "espressif",
    "framework-arduinoespressif32-libs": "espressif",
    "tool-esptoolpy": "espressif",
    "tool-mkspiffs": "igrr",
    "tool-mklittlefs": "earlephilhower",
    "tool-cmake": "platformio",
    "tool-ninja": "platformio",
    "tool-scons": "platformio",
    "toolchain-xtensa-esp-elf": "espressif",
    "espressif32": "platformio",
}

CORE = "piocore"
changed = []

for base in ("platforms", "packages"):
    d = os.path.join(CORE, base)
    if not os.path.isdir(d):
        continue
    for name in sorted(os.listdir(d)):
        marker = os.path.join(d, name, ".piopm")
        if not os.path.isfile(marker):
            continue
        with io.open(marker, encoding="utf-8") as fh:
            data = json.load(fh)
        owner = OWNERS.get(name)
        if owner and data.get("spec", {}).get("owner") != owner:
            data.setdefault("spec", {})["owner"] = owner
            with io.open(marker, "w", encoding="utf-8", newline="\n") as fh:
                json.dump(data, fh, indent=2)
            changed.append(f"{base}/{name} owner={owner}")

print("updated:")
for c in changed:
    print("  ", c)
if not changed:
    print("   (nothing to change)")
