p=r"C:\Users\DuWeisi\digimon-color-passport\tools\r6b\probe.cpp"
s=open(p,encoding='utf-8').read()
s=s.replace('\tconst char *evt_path = nullptr;','\tconst char *evt_path = nullptr;\n\tconst char *pc_dump_path = nullptr;',1)
s=s.replace('\t\telse if (a == "--events") evt_path = nx();','\t\telse if (a == "--events") evt_path = nx();\n\t\telse if (a == "--pc-dump") pc_dump_path = nx();',1)
# dump per-PC table at the very end (before return 0)
s=s.replace('\tif (g_f_csv)  std::fclose(g_f_csv);','''\tif (pc_dump_path)
\t{
\t\tFILE *fp = std::fopen(pc_dump_path, "w");
\t\tif (fp)
\t\t{
\t\t\tstd::fprintf(fp, "# pc cyc insn\\n");
\t\t\tfor (uint32_t a = 0; a < 0x400000u; ++a)
\t\t\t\tif (g_cyc_pc[a] || g_cnt_pc[a])
\t\t\t\t\tstd::fprintf(fp, "%06X %llu %llu\\n", a,
\t\t\t\t\t             (unsigned long long)g_cyc_pc[a], (unsigned long long)g_cnt_pc[a]);
\t\t\tstd::fclose(fp);
\t\t}
\t}

\tif (g_f_csv)  std::fclose(g_f_csv);''',1)
open(p,'w',encoding='utf-8',newline='\n').write(s)
print("ok")
