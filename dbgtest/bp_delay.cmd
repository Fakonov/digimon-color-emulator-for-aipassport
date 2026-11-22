bpset 5376
go
printf "ENTRY r4=%04X bp=%04X sr=%04X sb=%X pc=%06X\n", r4, bp, sr, sb, pc
save C:\Users\DuWeisi\digimon-color-passport\dbgtest\delay_entry_ram.bin,0,1000
bpclear
bpset 537f
go
printf "EXIT r4=%04X bp=%04X sr=%04X sb=%X pc=%06X\n", r4, bp, sr, sb, pc
save C:\Users\DuWeisi\digimon-color-passport\dbgtest\delay_exit_ram.bin,0,1000
quit
