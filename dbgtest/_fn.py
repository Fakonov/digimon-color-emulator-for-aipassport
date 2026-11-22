import io
s = io.open("emu/unsp_glue.cpp", encoding="utf-8").read().splitlines()
for i in range(870, 900):
    if i < len(s):
        print("%4d: %s" % (i+1, s[i]))
print("---- declarations of aot_note_block:")
for i, ln in enumerate(s):
    if "aot_note_block" in ln:
        print("%4d: %s" % (i+1, ln))
