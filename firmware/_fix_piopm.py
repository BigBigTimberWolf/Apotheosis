"""Rewrite .piopm markers with correct types and no BOM."""
import io, json, os

markers = {
    r"piocore\platforms\espressif32\.piopm": {
        "type": "platform",
        "name": "espressif32",
        "version": "6.7.0",
        "spec": {"owner": "platformio", "id": None, "name": "espressif32",
                 "requirements": None, "uri": None},
    },
    r"piocore\packages\toolchain-xtensa-esp32s3\.piopm": {
        "type": "tool",
        "name": "toolchain-xtensa-esp32s3",
        "version": "8.4.0+2021r2-patch5",
        "spec": {"owner": "espressif", "id": None,
                 "name": "toolchain-xtensa-esp32s3",
                 "requirements": None, "uri": None},
    },
}

for path, data in markers.items():
    with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2)
    # verify it parses and has no BOM
    raw = open(path, "rb").read()
    assert not raw.startswith(b"\xef\xbb\xbf"), f"BOM in {path}"
    json.loads(raw.decode("utf-8"))
    print(f"  ok {path}  type={data['type']} v={data['version']}")
print("done")
