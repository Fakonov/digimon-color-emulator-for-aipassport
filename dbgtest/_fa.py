import io
p = r"tools/aot.py"
s = io.open(p, encoding="utf-8").read()
i = s.find("covered = set()")
print(s[i-700:i+900])
