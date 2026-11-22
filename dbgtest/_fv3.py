import io
p = r"tools/aot.py"
lines = io.open(p, encoding="utf-8").read().splitlines()
for i, ln in enumerate(lines):
    if "MIN_NATIVE_INSNS" in ln or ("native <" in ln):
        for j in range(max(0,i-8), min(len(lines), i+10)):
            print("%4d: %s" % (j+1, lines[j]))
        print("---")
