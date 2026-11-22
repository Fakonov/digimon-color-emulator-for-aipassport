// Single translation unit for the whole unSP core.
//
// The instruction set lives in five MAME-derived files and the dispatch glue in
// unsp_glue.cpp.  Compiled separately, `execute_one -> execute_remaining ->
// do_basic_alu_ops` is a cross-unit call chain: every interpreted instruction
// pays a full register save/restore round trip, which measured 228 host cycles
// on the ESP32-C3.  Including them into one unit lets -O3 inline the chain.
//
// The included files are the unmodified copies (build.ps1 / CMakeLists.txt copy
// ref/unsp/*.cpp into emu/isa/), so the upstream text stays readable.

#include "unsp_extended.cpp"
#include "unsp_exxx.cpp"
#include "unsp_fxxx.cpp"
#include "unsp_jumps.cpp"
#include "unsp_other.cpp"
#include "../unsp_glue.cpp"
