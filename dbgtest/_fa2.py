import io
# 1) global duplicate guard in the generator
p = r"tools/aot.py"
s = io.open(p, encoding="utf-8").read()
old = """    for entry, is_entry in todo:
        if not is_entry and entry in covered and entry n"""
i = s.find(old)
j = s.find("\n", s.find("continue", i))
seg = s[i:j+1]
print("--- current loop head ---")
print(seg)
