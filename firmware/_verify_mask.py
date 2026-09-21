"""
Verify the momentary-mask state machine's safety properties.

Models fw_host's mask logic exactly as implemented:
  - maskStart() snapshots device-side belief (maskLast*), refuses re-arm
  - maskStop() releases held buttons + sends empty keyboard snapshot,
    then invalidates the keyboard dedup cache
  - the dedup cache is invalidated so a still-held key re-reports

Checks:
  1. hard timeout always fires
  2. duration clamped
  3. re-arm refused (no indefinite lockout)
  4. device-side belief is NOT updated during the mask
  5. held buttons/keys released on stop
  6. still-held key re-reports after the mask (dedup invalidated)
  7. movement dropped, no compensation needed
  8. normal passthrough unaffected
"""

MAX_MS = 2000
DEFAULT_MS = 50
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_FORWARD, BTN_BACKWARD = 1, 2, 4, 16, 8


class Host:
    def __init__(self):
        self.now = 0
        self.mask_until = 0
        self.out = []
        # device-side belief (what we last told the device)
        self.last_buttons = 0
        self.kbd_mod = 0
        self.kbd_keys = [0] * 6
        # mask snapshot
        self.held_valid = False
        self.mask_last_buttons = 0
        self.mask_last_mod = 0
        self.mask_last_keys = [0] * 6
        # dedup cache (as if reset at boot)
        self.dedup_mod = 0xFF
        self.dedup_keys = [0xFF] * 6
        self.dedup_count = 0xFF

    def mask_active(self):
        return self.mask_until != 0

    def mask_start(self, ms):
        if ms == 0:
            ms = DEFAULT_MS
        if ms > MAX_MS:
            ms = MAX_MS
        if self.mask_active():
            return None
        self.held_valid = True
        self.mask_last_buttons = self.last_buttons
        self.mask_last_mod = self.kbd_mod
        self.mask_last_keys = list(self.kbd_keys)
        self.mask_until = self.now + ms
        if self.mask_until == 0:
            self.mask_until = 1
        return ms

    def mask_stop(self):
        if not self.mask_active():
            return
        if self.held_valid:
            for name, bit in (("left", BTN_LEFT), ("right", BTN_RIGHT),
                              ("middle", BTN_MIDDLE), ("side1", BTN_FORWARD),
                              ("side2", BTN_BACKWARD)):
                if self.mask_last_buttons & bit:
                    self.out.append(f"km.{name}(0)")
            any_held = self.mask_last_mod != 0 or any(self.mask_last_keys)
            if any_held:
                self.out.append("KB_EMPTY")
            self.held_valid = False
        self.mask_until = 0
        self.last_buttons = 0
        self.kbd_mod = 0
        self.kbd_keys = [0] * 6
        # invalidate dedup cache -> next frame always goes through
        self.dedup_mod = 0xFF
        self.dedup_keys = [0xFF] * 6
        self.dedup_count = 0xFF

    def tick(self):
        if self.mask_active() and self.now >= self.mask_until:
            self.mask_stop()

    def on_buttons(self, b):
        if self.mask_active():
            self.last_buttons = b        # NOTE: snapshot NOT updated
            return
        if b != self.last_buttons:
            self.out.append(f"BTN {b:#x}")
            self.last_buttons = b

    def on_move(self, dx, dy):
        if self.mask_active():
            return
        self.out.append(f"MOVE {dx},{dy}")

    def on_keys(self, mod, keys):
        keys = list(keys) + [0] * (6 - len(keys))
        if self.mask_active():
            return                        # NOTE: snapshot NOT updated
        changed = (mod != self.dedup_mod or len(keys) != self.dedup_count
                   or any(keys[i] != self.dedup_keys[i] for i in range(6)))
        if not changed:
            return
        self.dedup_mod, self.dedup_count, self.dedup_keys = mod, len(keys), keys
        self.kbd_mod, self.kbd_keys = mod, keys
        self.out.append(f"KB {mod:#x} {[hex(k) for k in keys if k]}")


def main():
    fails = []

    def check(name, cond, detail=""):
        if cond:
            print(f"  ok   {name}")
        else:
            print(f"  FAIL {name} {detail}")
            fails.append(name)

    h = Host(); h.mask_start(50)
    for _ in range(200):
        h.now += 1; h.tick()
    check("hard timeout fires without software help", not h.mask_active())

    h = Host(); applied = h.mask_start(999999)
    check("duration clamped to max", applied == MAX_MS, f"got {applied}")
    h.now += MAX_MS; h.tick()
    check("clamped mask still expires", not h.mask_active())

    h = Host(); h.mask_start(50); first = h.mask_until
    check("re-arm while active refused",
          h.mask_start(50) is None and h.mask_until == first)

    # 4. belief frozen during mask
    h = Host()
    h.on_buttons(BTN_LEFT | BTN_RIGHT)
    h.mask_start(100)
    h.on_buttons(0)
    check("device belief frozen during mask",
          h.mask_last_buttons == (BTN_LEFT | BTN_RIGHT), hex(h.mask_last_buttons))

    # 5. held buttons released
    h.now += 100; h.tick()
    check("held mouse buttons released",
          "km.left(0)" in h.out and "km.right(0)" in h.out, str(h.out))
    check("movement dropped during mask",
          not any(x.startswith("MOVE") for x in h.out), str(h.out))

    # 4b. held keys released
    h = Host()
    h.on_keys(0x02, [0x04])
    h.mask_start(80)
    h.on_keys(0x00, [])
    h.now += 80; h.tick()
    check("held keyboard keys released", "KB_EMPTY" in h.out, str(h.out))

    # 6. still-held key re-reports after mask (dedup invalidated)
    h = Host()
    h.on_keys(0x00, [0x04])          # press 'a'
    h.mask_start(50)
    h.now += 50; h.tick()            # mask ends, 'a' physically still held
    h.out.clear()
    h.on_keys(0x00, [0x04])          # keyboard re-sends same snapshot
    check("still-held key re-reports after mask",
          len(h.out) == 1 and h.out[0].startswith("KB 0x0"), str(h.out))

    # 7. no spurious frames when nothing held
    h = Host(); h.mask_start(50)
    h.now += 50; h.tick()
    check("no spurious releases when nothing held", h.out == [], str(h.out))

    # 7b. device state converges
    h = Host()
    h.on_buttons(BTN_LEFT)
    h.on_keys(0x00, [0x04])
    h.mask_start(30)
    h.on_buttons(0); h.on_keys(0x00, [])
    h.now += 30; h.tick()
    check("device key state converges to released",
          h.out == ["BTN 0x1", "KB 0x0 ['0x4']", "km.left(0)", "KB_EMPTY"],
          str(h.out))

    # 8. normal passthrough
    h = Host()
    h.on_buttons(BTN_LEFT); h.on_move(10, -5); h.on_keys(0x00, [0x04]); h.on_buttons(0)
    check("normal passthrough unaffected",
          h.out == ["BTN 0x1", "MOVE 10,-5", "KB 0x0 ['0x4']", "BTN 0x0"],
          str(h.out))

    # 9. modifier-only hold
    h = Host(); h.on_keys(0x02, [])
    h.mask_start(50); h.now += 50; h.tick()
    check("modifier-only hold released", "KB_EMPTY" in h.out, str(h.out))

    print()
    print("ALL PASS" if not fails else f"FAILURES: {fails}")
    return 0 if not fails else 1


if __name__ == "__main__":
    raise SystemExit(main())
