p=r"C:\Users\DuWeisi\digimon-color-passport\tools\r6b\probe.cpp"
s=open(p,encoding='utf-8').read()
s=s.replace('\tconst char *csv_path = nullptr, *blit_path = nullptr, *win_path = nullptr, *evt_path = nullptr;',
            '\tconst char *csv_path = nullptr, *blit_path = nullptr, *win_path = nullptr, *evt_path = nullptr;\n\tconst char *pc_dump_path = nullptr;',1)
open(p,'w',encoding='utf-8',newline='\n').write(s)
print('ok')
