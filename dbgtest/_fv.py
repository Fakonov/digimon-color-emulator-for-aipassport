import io
s = io.open("tools/aot.py", encoding="utf-8").read()
for key in ("MIN_NATIVE_INSNS", "skipped_direct", "is_entry"):
    i = 0
    n = 0
    while n < 4:
        i = s.find(key, i)
        if i < 0: break
        ln0 = s.rfind("\n", 0, i) + 1
        ln1 = s.find("\n", i)
        print("%-18s %s" % (key, s[ln0:ln1]))
        i = ln1; n += 1
    print()
