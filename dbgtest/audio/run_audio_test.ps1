# Build and run the DAC/PPAM audio model selftest (docs/AUDIO.md §2).
#   powershell -NoProfile -ExecutionPolicy Bypass -File dbgtest\audio\run_audio_test.ps1
# Requires emu\build.ps1 to have run at least once (it produces emu/obj/isa_*.o
# and emu/obj/unsp_glue.o, which the SoC object needs for linking).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
Set-Location $root

$g = "C:\Users\DuWeisi\w64devkit\bin\g++.exe"
$flags = @("-std=c++20", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-Wno-sign-compare", "-I", "emu/compat", "-I", "emu")
$objs = @(Get-ChildItem emu/obj/isa_*.o | ForEach-Object { $_.FullName }) + @("emu/obj/unsp_glue.o")

& $g @flags -o emu/audio_selftest.exe emu/audio_selftest.cpp emu/gpce4.cpp emu/st7735.cpp emu/audio_dump.cpp @objs
if ($LASTEXITCODE -ne 0) { throw "compile/link failed" }

& emu/audio_selftest.exe
exit $LASTEXITCODE
