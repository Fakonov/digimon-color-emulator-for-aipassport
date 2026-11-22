import re

files = ["roms/internal_rom_221017a.bin", "roms/dmc_v1_base.bin"]
pats = {0x3040: "DAC_Ctrl", 0x3041: "DAC_CH1", 0x3042: "DAC_CH2", 0x3043: "PPAMCtrl",
        0x3010: "TimerA_Data", 0x3011: "TimerA_Cnt", 0x3016: "Timer_Ctrl", 0x3020: "PWM0"}

for f in files:
    d = open(f, "rb").read()
    print(f, len(d), "bytes")
    for w, name in pats.items():
        b = bytes([w & 0xff, (w >> 8) & 0xff])
        hits = [m.start() for m in re.finditer(re.escape(b), d)]
        print("   0x%04x %-12s %d hit(s) %s" % (w, name, len(hits), [hex(h) for h in hits[:8]]))
