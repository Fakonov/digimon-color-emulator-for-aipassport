"""Route the diagnostic auto-press through the normal hold path.

The first version set s_hold_mask/s_hold_left and then skipped update_buttons()
for that frame - but the following frames called update_buttons() again, which
immediately cleared the mask because nothing was physically held.  The guest saw
a one-frame press, which it ignores by design.  Now the auto-press is applied at
the end of update_buttons(), so it survives the same four-frame rule a real tap
does.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"

OLD_CALL = """        if (kAutoPressAt != 0 && s_frames == kAutoPressAt) {
            ESP_LOGI(TAG, "auto press B1 at frame %u", unsigned(s_frames));
            s_hold_mask = 0x0008;
            s_hold_left = kPressFrames;
        } else {
            update_buttons();
        }"""

NEW_CALL = "        update_buttons();"

ANCHOR = "    const int mask = instant != 0 ? instant : (s_hold_left > 0 ? s_hold_mask : 0);"
INJECT = ("    // diagnostic: emulate a physical B1 hold for a few guest frames\n"
          "    if (kAutoPressAt != 0 && s_frames >= kAutoPressAt && s_frames < kAutoPressAt + 4) {\n"
          "        s_hold_mask = 0x0008;\n"
          "        s_hold_left = kPressFrames;\n"
          "    }\n")


def main():
    s = io.open(PATH, encoding="utf-8").read()
    if OLD_CALL in s:
        s = s.replace(OLD_CALL, NEW_CALL, 1)
        print("reverted the bypassing auto-press")
    if "diagnostic: emulate a physical B1 hold" in s:
        print("hold injection already present")
    else:
        if ANCHOR not in s:
            raise SystemExit("mask anchor missing")
        s = s.replace(ANCHOR, INJECT + ANCHOR, 1)
        print("hold injection added")
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
