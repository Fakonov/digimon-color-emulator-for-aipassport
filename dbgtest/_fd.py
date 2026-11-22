import io
s = io.open("firmware/digicore-c3/main/display.c", encoding="utf-8").read().splitlines()
for i, ln in enumerate(s):
    if "draw_bitmap" in ln or "swap_xy" in ln or "mirror" in ln or "set_gap" in ln:
        print("%4d: %s" % (i+1, ln.rstrip()))
