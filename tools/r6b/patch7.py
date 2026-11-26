p=r"C:\Users\DuWeisi\digimon-color-passport\tools\r6b\probe.cpp"
s=open(p,encoding='utf-8').read()
s=s.replace("\tbool clock_sync = false;","\tbool clock_sync = false;\n\tint64_t clock_frame = 120;",1)
s=s.replace('\t\telse if (a == "--clock-sync") clock_sync = true;','\t\telse if (a == "--clock-sync") clock_sync = true;\n\t\telse if (a == "--clock-frame") clock_frame = std::atoll(nx());',1)
s=s.replace("		if (clock_sync && f == 120)","		if (clock_sync && f == clock_frame)",1)
open(p,'w',encoding='utf-8',newline='\n').write(s)
print('ok')
