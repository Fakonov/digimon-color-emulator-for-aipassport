gtime #2000
bpset 2002f4
go
printf "ENTRY SB=%X SR=%04X\n", sb, sr
save C:\Users\DuWeisi\digimon-color-passport\dbgtest\entry_ram.bin,0,1000
bpset 200343
go
printf "EXIT SB=%X SR=%04X\n", sb, sr
save C:\Users\DuWeisi\digimon-color-passport\dbgtest\exit_ram.bin,0,1000
quit
