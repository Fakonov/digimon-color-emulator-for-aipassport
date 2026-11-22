import io
s = io.open("firmware/digicore-c3/main/app_main.cpp", encoding="utf-8").read()
i = s.find("blit_frame")
print(s[i-200:i+3000])
