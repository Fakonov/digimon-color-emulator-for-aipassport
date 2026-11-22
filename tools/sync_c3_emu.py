#!/usr/bin/env python
"""Sync the shared emulator sources into the ESP32-C3 project.

`firmware/digicore-c3/main/emu/` is a copy of `emu/` that carries three
C3-only additions:

  * `m_insn_count` / `take_insns()` in `compat/unsp.h` (guest instruction rate)
  * one counter bump per executed instruction inside `execute_run()`
  * no per-instruction cycle hook (not needed on device, but harmless)

Rather than hand-editing the copy every time the PC emulator gains an
optimization, regenerate it here: copy, then re-insert the C3 bits and fail
loudly if an insertion point disappeared.
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "emu")
DST = os.path.join(ROOT, "firmware", "digicore-c3", "main", "emu")

INSNS_FIELD = """
	// Guest instruction counter.  execute_run() bumps this once per executed
	// instruction; the harness folds it into a 64-bit total per frame (32 bits
	// never wraps inside one frame - a frame is < 2 M instructions).  One
	// increment is much cheaper than a 64-bit add on RV32IMC.
	uint32_t m_insn_count = 0;
	uint32_t take_insns()
	{
		const uint32_t n = m_insn_count;
		m_insn_count = 0;
		return n;
	}
"""


def read(path):
    with open(path, encoding="utf-8") as fp:
        return fp.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(text)


# The device tree carries hand-written code the PC tree does not have: the AOT
# blit/push HLE hooks and their pixel counters in unsp_glue.cpp, the C3_AOTACC
# access counters and the page-switch counter in aot.h.  Copying the PC file
# over them silently deleted 265 lines once and only surfaced as an undefined
# reference at link time, so every file this script overwrites is checked first.
DEVICE_ONLY = {
    "unsp_glue.cpp": ("g_blit_px_win", "g_hle_push_px_win"),
    "aot.h": ("C3_AOTACC", "g_aot_pg_switch"),
}


def refuse_clobber(name, src_text):
    dst = os.path.join(DST, name)
    if not os.path.exists(dst):
        return
    existing = read(dst)
    for marker in DEVICE_ONLY.get(name, ()):
        if marker in existing and marker not in src_text:
            sys.exit("%s: the device copy has %r but the source does not -- "
                     "merge the change by hand instead of overwriting" % (name, marker))


def sync_unsp_h():
    text = read(os.path.join(SRC, "compat", "unsp.h"))
    anchor = "\tUnspBus *m_bus;"
    if anchor not in text:
        sys.exit("unsp.h: anchor %r missing" % anchor)
    text = text.replace(anchor, INSNS_FIELD + "\n" + anchor, 1)
    write(os.path.join(DST, "compat", "unsp.h"), text)


def sync_glue():
    text = read(os.path.join(SRC, "unsp_glue.cpp"))
    refuse_clobber("unsp_glue.cpp", text)
    text = text.replace(
        '#include "hotblock.h"',
        '//\n// C3 port: execute_run() also bumps the guest instruction counter, which is\n'
        '// what the firmware reports as mi_s (million guest instructions per second).\n'
        '#include "hotblock.h"',
        1,
    )

    hot_marker = "\t\t\t\tcore->m_icount -= (int)HOT_BLOCK_CYCLES;"
    if hot_marker not in text:
        sys.exit("glue: hot block charge line missing")
    text = text.replace(
        hot_marker,
        hot_marker + "\n\t\t\t\tm_insn_count += 68; // 0x2002F4..0x200342",
        1,
    )

    # ------------------------------------------------------------------ counts
    # Guest instruction counting has to cover every way an instruction retires:
    #
    #   (a) an AOT block retires `cnt` of them in one call;
    #   (b) aot_fallback_one() executes exactly one;
    #   (c) the single-instruction fast path leaves through `continue`;
    #   (d) the interpreter's own execute_one() sites.
    #
    # (c) and (d) are exclusive - the fast path never reaches (d) - so nothing
    # is counted twice.  The g_verify branch also calls execute_one(), but that
    # replay runs after `*core = save;` restores the pre-instruction state and
    # is verification scaffolding, not a retired instruction.
    aot_marker = "\tm_aot_insns += cnt;"
    if aot_marker not in text:
        sys.exit("glue: AOT instruction count line missing")
    text = text.replace(aot_marker, aot_marker + "\n\tm_insn_count += cnt;", 1)

    fb_marker = "\td->m_aot_fallback++;"
    if fb_marker not in text:
        sys.exit("glue: AOT fallback line missing")
    text = text.replace(fb_marker, fb_marker + "\n\td->m_insn_count++;", 1)

    fast_marker = ("\t\t\tif (m_fastops && (fast_step(op) ? (++g_fast_hits, true) : false))\n"
                   "\t\t\t{\n")
    if fast_marker not in text:
        sys.exit("glue: fastops branch missing")
    text = text.replace(fast_marker, fast_marker + "\t\t\t\tm_insn_count++;\n", 1)

    calls = [m for m in re.finditer(r"^(\t+)execute_one\(op\);$", text, re.M)
             if "*core = save;" not in text[max(0, m.start() - 120):m.start()]]
    if len(calls) != 3:
        sys.exit("glue: expected 3 counted execute_one() call sites, found %d" % len(calls))
    out = []
    last = 0
    for m in calls:
        out.append(text[last : m.end()])
        out.append("\n" + m.group(1) + "m_insn_count++;")
        last = m.end()
    out.append(text[last:])
    write(os.path.join(DST, "unsp_glue.cpp"), "".join(out))


def sync_aot():
    """Copy aot.h and regenerate the device block tables (flash-resident)."""
    import subprocess

    refuse_clobber("aot.h", read(os.path.join(SRC, "aot.h")))
    shutil.copyfile(os.path.join(SRC, "aot.h"), os.path.join(DST, "aot.h"))
    env = dict(os.environ)
    env["AOT_CONST_TABLES"] = "1"
    env.pop("AOT_NO_EXTRA", None)
    env["AOT_LOOSE"] = "1"
    env["AOT_OUT"] = os.path.join(DST, "aot_blocks.c")
    # The handset runs scenes the PC sampler never visits, so the device list is
    # the static one plus every PC the miss census collected on hardware.
    dev_list = os.path.join(ROOT, "tools", "coverage", "out", "AB_dev.txt")
    if os.path.exists(dev_list):
        env["AOT_EXTRA"] = dev_list
    else:
        env.pop("AOT_EXTRA", None)
    # The de-duplication pass only registers block entries, so a jump into the
    # middle of an existing block misses the flash table on hardware.  Every PC
    # the device miss census named gets its own entry (AOT_FORCE).
    # IRAM placement comes from the device block census, not the PC sampler: the
    # moving scenes hit a completely different set of blocks.  Top 60 covers 97%
    # of dispatches and costs ~48 KB of the ~57 KB that is free.
    iram_list = os.path.join(ROOT, "tools", "coverage", "out", "aot_iram_dev60.txt")
    if os.path.exists(iram_list):
        env["AOT_IRAM_LIST"] = iram_list
    else:
        env.pop("AOT_IRAM_LIST", None)
    force_list = os.path.join(ROOT, "tools", "coverage", "out", "AB_force.txt")
    if os.path.exists(force_list):
        env["AOT_FORCE"] = force_list
    else:
        env.pop("AOT_FORCE", None)
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "aot.py")], env=env)


def main():
    shutil.copyfile(os.path.join(SRC, "hotblock.h"), os.path.join(DST, "hotblock.h"))
    sync_unsp_h()
    sync_glue()
    sync_aot()
    print("synced hotblock.h, aot.h, aot_blocks.c, compat/unsp.h, unsp_glue.cpp into %s" % DST)
    return 0


if __name__ == "__main__":
    sys.exit(main())
