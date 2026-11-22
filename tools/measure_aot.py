"""Compile one aot_blocks.c variant standalone and report code size + call pressure.

Usage: python tools/measure_aot.py SRC.c OUT.obj TAG
Reuses the real compile command from build/compile_commands.json so the numbers
are comparable with the firmware build.
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "firmware", "digicore-c3")
CC = os.path.join(FW, "build", "compile_commands.json")
BIN = r"C:\Users\DuWeisi\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin"


def main():
    src = os.path.abspath(sys.argv[1])
    obj = os.path.abspath(sys.argv[2])
    tag = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(src)

    entries = json.load(open(CC, encoding="utf-8"))
    ent = [e for e in entries if e["file"].endswith("aot_blocks.c")][0]
    cmd = ent["command"]

    # swap input and output, keep every flag
    cmd = re.sub(r"-o\s+\S+", "-o " + obj.replace("\\", "/"), cmd, count=1)
    cmd = re.sub(r"-c\s+\S+", "-c " + src.replace("\\", "/"), cmd, count=1)
    # static block functions are unreferenced in a sliced TU, so -O3 would drop
    # them as dead code; -fkeep-static-functions keeps the body for measurement.
    if "-fkeep-static-functions" not in cmd:
        cmd += " -fkeep-static-functions"

    # Windows caps a shell command line at 8191 chars; this one is longer, so
    # hand the compiler an argv list (shell=False) instead of a command string.
    import shlex
    argv = shlex.split(cmd, posix=False)
    r = subprocess.run(argv, cwd=os.path.join(FW, "build"),
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-4000:])
        print(r.stderr[-4000:])
        sys.exit("compile failed for " + tag)

    nm = subprocess.run([os.path.join(BIN, "riscv32-esp-elf-nm.exe"),
                         "--print-size", "--size-sort", "--radix=d", obj],
                        capture_output=True, text=True).stdout
    syms = []
    for line in nm.splitlines():
        p = line.split()
        if len(p) != 4:
            continue
        try:
            adr, size = int(p[0]), int(p[1])
        except ValueError:
            continue
        syms.append((p[3], adr, size))

    blocks = [s for s in syms if "aot_b" in s[0]]
    total = sum(s[2] for s in blocks)
    biggest = max((s[2] for s in blocks), default=0)

    dis = subprocess.run([os.path.join(BIN, "riscv32-esp-elf-objdump.exe"),
                          "-d", "--no-show-raw-insn", obj],
                         capture_output=True, text=True).stdout
    n_insn = n_jal = n_jalr = n_ret = n_load = n_store = 0
    calls = {}
    for line in dis.splitlines():
        m = re.match(r"\s+[0-9a-f]+:\s+(\S+)\s*(.*)", line)
        if not m:
            continue
        op, args = m.group(1), m.group(2)
        n_insn += 1
        if op in ("jal", "c.jal"):
            n_jal += 1
            t = args.split(",")[-1].strip()
            if t:
                calls[t] = calls.get(t, 0) + 1
        elif op in ("jalr", "c.jalr"):
            n_jalr += 1
        elif op in ("ret", "c.ret"):
            n_ret += 1
        elif op.startswith("l") and op not in ("lui", "li", "la"):
            n_load += 1
        elif op.startswith("s") and op not in ("slli", "srai", "srli", "sub", "seqz", "snez", "slt", "slti", "sltu"):
            n_store += 1

    print("=" * 62)
    print("tag            :", tag)
    print("blocks         : %d" % len(blocks))
    print("block code     : %d B  (avg %d B/block, max %d B)"
          % (total, total // max(len(blocks), 1), biggest))
    print("insns          : %d" % n_insn)
    print("  jal          : %d   (%.1f%% of insns)" % (n_jal, 100.0 * n_jal / max(n_insn, 1)))
    print("  jalr         : %d" % n_jalr)
    print("  ret          : %d" % n_ret)
    print("  load         : %d" % n_load)
    print("  store        : %d" % n_store)
    print("top call targets:")
    for k, v in sorted(calls.items(), key=lambda kv: -kv[1])[:10]:
        print("   %-40s %d" % (k, v))
    print("size/mnemonic histogram:")
    hist = {}
    for line in dis.splitlines():
        m = re.match(r"\s+[0-9a-f]+:\s+(\S+)", line)
        if m:
            hist[m.group(1)] = hist.get(m.group(1), 0) + 1
    for k, v in sorted(hist.items(), key=lambda kv: -kv[1])[:18]:
        print("   %-12s %d" % (k, v))


if __name__ == "__main__":
    main()
