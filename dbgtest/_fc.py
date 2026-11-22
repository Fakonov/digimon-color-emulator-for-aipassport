import io
p = r"tools/c3_serial_capture.py"
s = io.open(p, encoding="utf-8").read()
if "reset_input_buffer" not in s:
    old = "p = serial.Serial("
    i = s.find(old)
    j = s.find("\n", i)
    # insert after the Serial(...) statement's closing line
    k = j + 1
    s = s[:k] + "p.reset_input_buffer()   # drop whatever the CDC buffer kept from a previous run\n" + s[k:]
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)
    print("flush added")
else:
    print("already there")
print(s[:1200])
