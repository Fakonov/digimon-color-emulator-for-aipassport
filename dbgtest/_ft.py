import io
p = r"firmware/digicore-c3/main/app_main.cpp"
s = io.open(p, encoding="utf-8").read()
i = s.find("    const int rows = last - first + 1;")
print(s[i:i+2600])
