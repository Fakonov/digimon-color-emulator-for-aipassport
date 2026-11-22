import io
s = io.open("firmware/digicore-c3/main/app_main.cpp", encoding="utf-8").read()
i = s.find("kSrcRowStride")
while i != -1 and i < len(s):
    end = s.find("\n", i)
    print(s[max(0,s.rfind("\n",0,i)+1):end])
    i = s.find("kSrcRowStride", end)
print("=== kScale / kWin / kChunkRows / kRot definitions:")
for key in ("kScale", "kWinX", "kWinY", "kWinW", "kWinH", "kChunkRows", "kRot", "kFbPanel"):
    j = s.find("constexpr int " + key)
    if j < 0: j = s.find("constexpr uint32_t " + key)
    if j >= 0:
        print(s[j:s.find("\n", j)])
