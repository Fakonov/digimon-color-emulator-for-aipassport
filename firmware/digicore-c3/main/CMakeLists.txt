# digicore-c3: the Digimon Color GPCE4 model + unSP interpreter on the C3.
#
# main/emu/* is a copy of emu/* (the PC harness) with three device-side edits:
#   gpce4.h/.cpp  SPI flash behind SparseRom instead of a host pointer
#   st7735.cpp    128x128 framebuffer only, no 256x256 GRAM
#   unsp_glue.cpp guest instruction counter for the mi_s log field
# emu/ itself is never touched (its owner keeps editing it); see docs/C3_PORT.md.
idf_component_register(
    SRCS "app_main.cpp"
         "rom_esp.cpp"
         "saveimg.cpp"
         "gueststate.cpp"
         "fbsave.cpp"
         "menu.cpp"
         "buttons.c"
         "audio.c"
         "display.c"
         "emu/gpce4.cpp"
         "emu/romstore.cpp"
         "emu/st7735.cpp"
         "emu/isa/unsp_amalgam.cpp"
         "emu/aot_blocks.c"
    INCLUDE_DIRS "." "emu" "emu/compat"
    REQUIRES spi_flash esp_partition esp_timer esp_lcd driver esp_adc esp_rom nvs_flash
)

# The MAME-derived copies (emu/gpce4.cpp, emu/unsp_glue.cpp) print uint32_t
# addresses through printf("%x"): on riscv32 uint32_t is `long unsigned int`,
# so -Werror=format (ESP-IDF default) rejects them.  Keeping the printfs as they
# are means re-syncing a copy from emu/ never needs a second edit pass; the
# warnings stay visible, they are just not fatal.
# ESP-IDF compiles with -fno-jump-tables -fno-tree-switch-conversion (good for
# size, terrible for the interpreter: every opcode switch becomes a compare
# chain).  The ISA decoder is one giant nested switch, so re-enable both for
# this component; the two flags must come after the IDF defaults to win.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format -O3 -DC3_PROF=1 -DC3_SPLIT=0 -DC3_SKIP_BLIT=0
                       -fjump-tables -ftree-switch-conversion
                       -DAOT_CONST_TABLES -DAOT_NO_HOOKS -DC3_MISS_CENSUS=0 -DC3_BLOCK_CENSUS=0 -DC3_NO_AOT=0
                       -DC3_BLIT_CYC=139 -DC3_AOTACC=1)

# aot_blocks.c is generated C++ (nullptr, the AotFn typedef) that keeps the .c
# name so the PC harness and tools/aot.py can name it without a second code path.
set_source_files_properties(emu/aot_blocks.c PROPERTIES LANGUAGE CXX)

# The block bodies are 673 KB against a 16 KB instruction cache, so a moving
# scene misses on essentially every dispatch.  -Os shrinks the bodies (fewer
# cache lines per block) at the cost of a few extra instructions each; if the
# miss penalty dominates, smaller wins.  Measured A/B, not assumed: see
# docs/ notes on C3_PERF.  Remove this line to go back to the -O3 default.
set_source_files_properties(emu/aot_blocks.c PROPERTIES COMPILE_OPTIONS "-O3")

# The 64 KiB internal mask ROM (roms/internal_rom_221017a.bin) is linked into
# the app image: target_add_binary_data generates an .S with .incbin and the
# symbols internal_rom / internal_rom_length, so the data stays in flash
# (.rodata.embedded) and costs no RAM.
target_add_binary_data(${COMPONENT_LIB} "assets/internal_rom_221017a.bin" BINARY
                       RENAME_TO internal_rom)

# version.txt is the single source of truth for the firmware version: the top
# level CMakeLists feeds it to PROJECT_VER (so it lands in the app descriptor
# and idf.py / esptool report it), and here it becomes a string macro the code
# can print without pulling in esp_app_format just to read its own descriptor.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../version.txt" DIGICORE_VER_RAW)
string(STRIP "${DIGICORE_VER_RAW}" DIGICORE_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE "DIGICORE_VERSION=\"${DIGICORE_VER}\"")
