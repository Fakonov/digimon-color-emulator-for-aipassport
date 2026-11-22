import io
s = io.open("firmware/digicore-c3/main/rom_esp.cpp", encoding="utf-8").read().splitlines()
for i in range(110, 145):
    if i < len(s):
        print("%4d: %s" % (i+1, s[i]))
