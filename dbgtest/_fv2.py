import io
p = r"tools/aot.py"
s = io.open(p, encoding="utf-8").read()
# Only real block entries get the short-block exemption; hot PCs must earn their place.
old = "            if native < MIN_NATIVE_INSNS:"
i = s.find(old)
print(s[i-500:i+400])
