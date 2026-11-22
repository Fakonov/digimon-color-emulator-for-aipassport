"""Put the interpreter's dispatch path into IRAM.

The device spends ~200 host cycles on a single interpreted instruction, which is
far more than the arithmetic deserves.  Every dispatch walks execute_one ->
execute_remaining -> do_basic_alu_ops, three cold functions whose bodies live in
SPI flash while the same SPI flash is also serving the guest ROM page cache -
each miss is a cache line fetched over a busy bus.

Moving the dispatch path into IRAM costs about 8 KB of the ~100 KB free DRAM and
removes the XIP round trip.  Measured with the same 60 s log and fps_deltas.py.
"""
import io
import os

FILES = [
    "firmware/digicore-c3/main/emu/isa/unsp_other.cpp",
    "firmware/digicore-c3/main/emu/isa/unsp_extended.cpp",
    "firmware/digicore-c3/main/emu/isa/unsp_exxx.cpp",
    "firmware/digicore-c3/main/emu/isa/unsp_fxxx.cpp",
    "firmware/digicore-c3/main/emu/isa/unsp_jumps.cpp",
    "firmware/digicore-c3/main/emu/unsp_glue.cpp",
]

MARK = "__attribute__((section(\".iram1.text\")))"


def patch(path):
    s = io.open(path, encoding="utf-8").read()
    if MARK in s:
        return 0
    head = (
        "// IRAM placement: dispatch runs for every guest instruction, and each\n"
        "// call into flash fights the ROM page cache for the same SPI bus.\n"
        "#if defined(ESP_PLATFORM)\n"
        "#define UNSP_HOT __attribute__((section(\".iram1.text\")))\n"
        "#else\n"
        "#define UNSP_HOT\n"
        "#endif\n"
    )
    inserted = 0
    for sig in ("void unsp_device::execute_one(",
                "void unsp_device::execute_remaining(",
                "void unsp_device::execute_extended_group(",
                "void unsp_device::execute_exxx_group(",
                "void unsp_device::execute_fxxx_group(",
                "void unsp_device::execute_fxxx_101_group(",
                "void unsp_device::execute_jumps(",
                "void unsp_device::execute_run(",
                "uint16_t unsp_device::fetch16(",
                "void unsp_device::check_irqs("):
        if sig in s:
            s = s.replace(sig, "UNSP_HOT " + sig, 1)
            inserted += 1
    if inserted == 0:
        return 0
    # The macro has to be defined before the first use.
    first = s.find("UNSP_HOT ")
    s = s[:first] + head + s[first:]
    io.open(path, "w", encoding="utf-8", newline="\n").write(s)
    return inserted


def main():
    total = 0
    for f in FILES:
        if not os.path.exists(f):
            print("missing", f)
            continue
        n = patch(f)
        total += n
        print("%-58s %d" % (f.split("/")[-1], n))
    print("hot functions marked:", total)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
