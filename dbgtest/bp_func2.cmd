gtime #10000
bpset 2002f4
go
printf "ENTRY2 r1=%04X r2=%04X r3=%04X r4=%04X bp=%04X sr=%04X sb=%X pc=%06X\n", r1, r2, r3, r4, bp, sr, sb, pc
save C:\Users\DuWeisi\digimon-color-passport\dbgtest\entry2_ram.bin,0,1000
bpset 200343
go
printf "EXIT2 r1=%04X r2=%04X r3=%04X r4=%04X bp=%04X sr=%04X sb=%X pc=%06X\n", r1, r2, r3, r4, bp, sr, sb, pc
save C:\Users\DuWeisi\digimon-color-passport\dbgtest\exit2_ram.bin,0,1000
quit
