param([switch]$Clean)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)))
$g = "C:\Users\DuWeisi\w64devkit\bin\g++.exe"
$cxxflags = @("-std=c++20", "-O2", "-DAOT_CONST_TABLES", "-Wall", "-Wextra", "-Wno-unused-parameter",
  "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-sign-compare",
  "-Wno-missing-field-initializers", "-I", "emu/compat", "-I", "emu")
& $g @cxxflags -c -o tools/r6b/probe.o tools/r6b/probe.cpp
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
$objs = @("emu/obj/isa_amalgam.o","emu/obj/aot_blocks.o","emu/obj/gpce4.o","emu/obj/st7735.o","emu/obj/audio_dump.o")
& $g -o tools/r6b/probe.exe tools/r6b/probe.o @objs
if ($LASTEXITCODE -ne 0) { throw "link failed" }
Write-Host "built tools/r6b/probe.exe"
