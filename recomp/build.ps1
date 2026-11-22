$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Python = "C:\Users\DuWeisi\py311\python.exe"
$Gcc = "C:\Users\DuWeisi\w64devkit\bin\gcc.exe"
$RvGcc = "C:\Users\DuWeisi\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe"

Set-Location $Root

Write-Host "== generate C =="
& $Python recomp/gen_recomp.py
& $Python recomp/gen_delay.py

Write-Host "== x86 hot_func =="
& $Gcc -O2 -std=c11 -Wall -Wextra -Irecomp recomp/harness.c recomp/hot_func.c -o recomp/hot_test.exe
& ./recomp/hot_test.exe dbgtest/entry_ram.bin dbgtest/exit_ram.bin recomp/out_ram.bin
& ./recomp/hot_test.exe dbgtest/entry2_ram.bin dbgtest/exit2_ram.bin recomp/out2_ram.bin 1

Write-Host "== x86 delay_loop =="
& $Gcc -O2 -std=c11 -Wall -Wextra -Irecomp recomp/delay_harness.c recomp/delay_func.c -o recomp/delay_test.exe
& ./recomp/delay_test.exe dbgtest/delay_entry_ram.bin dbgtest/delay_exit_ram.bin recomp/delay_out_ram.bin

Write-Host "== RISC-V assembly =="
& $RvGcc -O2 -march=rv32imc -mabi=ilp32 -fno-pic -S recomp/hot_func.c -Irecomp -o recomp/hot_func_rv32.s
& $RvGcc -O2 -march=rv32imc -mabi=ilp32 -fno-pic -S recomp/delay_func.c -Irecomp -o recomp/delay_func_rv32.s
& $Python recomp/count_asm.py recomp/hot_func_rv32.s recomp/delay_func_rv32.s