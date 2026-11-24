#!/usr/bin/env python3
"""Generate the AOT block table and block bodies for the unSP core.

Input:  tools/coverage/out/AB_blocks.txt  (hex block entry PCs, one per line)
        roms/dmc_v1_base.bin, roms/internal_rom_221017a.bin
Output: emu/aot_blocks.c

Scope (docs/PLAN_AOT.md 4, step T2): every instruction form the interpreter
decodes in unsp_other.cpp / unsp_jumps.cpp, plus CALL16 / JMPF / JMPR from
unsp_fxxx.cpp.  Anything else truncates the block and is executed by
aot_fallback_one(), one instruction at a time.

The emitted code is a literal transcription of the interpreter:
  ref/unsp/unsp_other.cpp   execute_remaining(), do_basic_alu_ops()
  ref/unsp/unsp_jumps.cpp   execute_jumps()
  ref/unsp/unsp_fxxx.cpp    CALL16 / JMPF / JMPR
Cycle charges are the values those routines subtract from m_icount.

Two invariants the emitters must keep (see docs/PLAN_AOT.md 4.5):

  * a block does not maintain r7 (the PC) while it runs, so every way out of a
    block has to publish the next PC explicitly, either with aot_set_pc() or by
    letting the last instruction write r6/r7 itself;
  * anything that writes r6 or r7 ends the block, because the next instruction
    address is no longer statically known.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "coverage"))

from unsp_static import Images  # noqa: E402

BLOCKS = os.environ.get("AOT_BLOCKS") or os.path.join(ROOT, "tools", "coverage", "out", "AB_blocks.txt")
FLASH = os.path.join(ROOT, "roms", "dmc_v1_base.bin")
IROM = os.path.join(ROOT, "roms", "internal_rom_221017a.bin")
# An explicit AOT_EXTRA="" means "no extra hot PCs" (the device build uses the
# static block list only); an unset AOT_EXTRA falls back to the default file.
_extra_env = os.environ.get("AOT_EXTRA")
if os.environ.get("AOT_NO_EXTRA"):
    _extra_env = ""
EXTRA = _extra_env if _extra_env is not None else \
    os.path.join(ROOT, "tools", "coverage", "out", "AB_pcs.txt")
# AOT_FORCE lists PCs that must stay dispatchable even when an earlier block
# already translated their instructions.  The greedy de-duplication pass below
# keeps the code size down but only registers block *entries*; a jump into the
# middle of an existing block then misses the table on the device.  The miss
# census names exactly those PCs, so they are re-emitted as their own entries.
FORCE = os.environ.get("AOT_FORCE") or ""
# Flash-resident tables for the device build (see AOT_CONST_TABLES in aot.h).
CONST_TABLES = bool(os.environ.get("AOT_CONST_TABLES"))
# AOT_LOOSE drops the per-instruction interrupt / cycle-budget guards and checks
# them once per block instead.  PC builds keep them (the four-window trace
# comparison needs instruction-exact slice boundaries); the device build takes
# the loose path because the guards are pure overhead when the interpreter is
# already at 60 fps.
LOOSE = bool(os.environ.get("AOT_LOOSE"))
OUT = os.environ.get("AOT_OUT") or os.path.join(ROOT, "emu", "aot_blocks.c")

MAX_BLOCK_INSNS = 256
MIN_NATIVE_INSNS = 1
# Blocks with at least this many translated instructions get the register
# localisation pass.  Keeping r0..r7 and sb in C locals costs ~20 stores on every
# way out, which only amortises once the block is long enough; a one-instruction
# block would spend more time saving than running, and 15% of the static blocks
# are exactly that short.
LOCAL_MIN_INSNS = int(os.environ.get("AOT_LOCAL_MIN") or 4)
PC_MASK = 0x3FFFFF
# Superblock grouping.  Blocks whose statically known successors form a region
# with a single entry are emitted as one C function: the intermediate exits skip
# the 10-register flush, the 10-register reload and the table walk, which is
# ~20 memory accesses plus a page lookup per block boundary.  The cycle charge
# and the interrupt test still happen at every block boundary (LAB_BC labels), so
# slice boundaries stay where they were - that is what keeps the frame CRC
# identical to the ungrouped build.
GROUP = bool(os.environ.get("AOT_GROUP"))
MAX_GROUP_BLOCKS = int(os.environ.get("AOT_GROUP_BLOCKS") or 4)
MAX_GROUP_INSNS = int(os.environ.get("AOT_GROUP_INSNS") or 48)
_AOT_CYC_RE = re.compile(r"^(\s*)AOT_CYC\(c, 0x[0-9A-Fa-f]+u, (\d+)\);$")

# Blocks listed in AOT_IRAM_LIST are placed in IRAM on the device (the C3 has
# ~200 KB of instruction RAM, far less than the 710 KB of generated code, so
# only the measured hot set can live there).
IRAM_SET = set()
_iram_path = os.environ.get("AOT_IRAM_LIST")
if _iram_path and os.path.exists(_iram_path):
    for _ln in open(_iram_path, encoding="utf-8"):
        _ln = _ln.strip()
        if _ln and not _ln.startswith("#"):
            IRAM_SET.add(int(_ln.split()[0], 16))


# ref/unsp/unsp_jumps.cpp: condition code -> taken predicate over SR bits.
COND = {
    0: "!(c->r[6] & AOT_C)",
    1: "(c->r[6] & AOT_C)",
    2: "!(c->r[6] & AOT_S)",
    3: "(c->r[6] & AOT_S)",
    4: "!(c->r[6] & AOT_Z)",
    5: "(c->r[6] & AOT_Z)",
    6: "!(c->r[6] & AOT_N)",
    7: "(c->r[6] & AOT_N)",
    8: "((c->r[6] & (AOT_Z | AOT_C)) != AOT_C)",
    9: "((c->r[6] & (AOT_Z | AOT_C)) == AOT_C)",
    10: "((c->r[6] & (AOT_Z | AOT_S)) != 0u)",
    11: "((c->r[6] & (AOT_Z | AOT_S)) == 0u)",
    12: "(((c->r[6] >> 9) & 1u) == ((c->r[6] >> 7) & 1u))",
    13: "(((c->r[6] >> 9) & 1u) != ((c->r[6] >> 7) & 1u))",
}

# Instructions the interpreter never writes a register for.
NO_WRITE_OP0 = (0x04, 0x0C, 0x0D)
# ALU op0 values do_basic_alu_ops() implements; 0x05/0x07 are illegal there.
ALU_OP0 = (0x00, 0x01, 0x02, 0x03, 0x04, 0x06, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D)

# Reasons a block had to be truncated, for the coverage report.
REASONS = {}


_NOTE_ON = True


def note(reason):
    if not _NOTE_ON:
        return
    REASONS[reason] = REASONS.get(reason, 0) + 1


# r0..r7 reach the emitted text as c->r[N]; to_locals() moves them into the
# block's local variables.  Only the single-index form is rewritten here:
# push/pop address a register *range* (c->r[4 - i]), which cannot become a C
# variable reference, so their emitter expands the loop instead.
_REG_REF = re.compile(r"c->r\[(\d)\]")
_PUSH_REF = re.compile(r"aot_push\(c, (\d+), ")
_POP_REF = re.compile(r"aot_pop\(c, (\d+)\)")


def to_locals(lines):
    """Rewrite a block body so that r0..r7 and sb live in C locals.

    The emitters stay written against c->r[] because that is the shape the
    interpreter uses and therefore the easiest thing to audit against it; this
    pass only relocates the accesses and makes every exit publish them again.
    The arithmetic is untouched -- which is exactly what the four-window trace
    comparison is there to prove.

    Every exit saves first: aot_set_pc() writes c->r[6]/c->r[7] directly, and
    the fallback path hands the block back to the interpreter, which reads
    c->r[].
    """
    out = []
    for ln in lines:
        ln = _REG_REF.sub(lambda m: "r" + m.group(1), ln)
        ln = ln.replace("c->sb", "sb")
        ln = _PUSH_REF.sub(lambda m: "aot_push_sr(c, r" + m.group(1) + ", ", ln)
        ln = _POP_REF.sub(lambda m: "aot_pop_sr(c, r" + m.group(1) + ")", ln)
        ln = ln.replace("aot_op(c, ", "aot_op_sr(r6, ")
        if "aot_set_pc(c, " in ln:
            ln = ln.replace("aot_set_pc(c, ", "AOT_SAVE_REGS(c); aot_set_pc(c, ")
        if "c->fallback(" in ln:
            ln = ln.replace("c->fallback(", "AOT_SAVE_REGS(c); c->fallback(")
        out.append(ln)
    return out


def ds_expr(opb):
    """UNSP_LREG_I(opb): SR[10..15] become address bits 16..21."""
    return "(((c->r[6] << 6) & 0x3f0000u) | c->r[%d])" % opb


def emit(img, pc, first, loc=False):
    """Translate one instruction, rewriting it for block-local registers if asked.

    Keeping the two steps apart means the emitters never have to know about the
    localisation: they always describe the interpreter's c->r[] accesses, and
    to_locals() is the single place that moves them into r0..r7.
    """
    insn = _emit(img, pc, first, loc)
    if insn is None or not loc:
        return insn
    lines, length, term = insn
    return (to_locals(lines), length, term)


def _emit(img, pc, first, loc=False):
    """Translate the single instruction at pc.

    Returns (lines, length_words, term) or None when the form is not covered.
    term is None (fall through), "done" (the lines already returned) or "end"
    (caller appends 'icount -= cyc; return;').
    """
    op = img.word(pc)
    if op is None:
        note("outside-image")
        return None

    op0 = (op >> 12) & 15
    opa = (op >> 9) & 7
    op1 = (op >> 6) & 7
    opn = (op >> 3) & 7
    opb = op & 7
    imm6 = op & 0x3F
    hook = "\t%s(c, 0x%06Xu);" % ("AOT_I" if first else "AOT_IH", pc)
    cyc_line = "\t\tcyc += {1:d};" if LOOSE else \
    "\t\tAOT_CYC(c, 0x{0:06X}u, {1:d});"

    # ---------------------------------------------------------------- fxxx
    if op0 == 0xF:
        if op1 == 2 and (op & 0xFFC0) == 0xFE80:  # JMPF
            imm = img.word(pc + 1)
            if imm is None:
                note("fxxx-jmpf-outside")
                return None
            tgt = ((op & 0x3F) << 16) | imm
            return ([hook,
                     "\tAOT_CYC(c, 0x%06Xu, 5);" % pc,
                     "\taot_set_pc(c, 0x%06Xu);" % tgt,
                     "\treturn;"], 2, "done")

        if op1 == 3 and (op & 0xFFC0) == 0xFEC0:  # JMPR: PC = r3, SR[0..5] = r4
            return ([hook,
                     "\tAOT_CYC(c, 0x%06Xu, 5);" % pc,
                     "\taot_set_pc(c, ((c->r[4] & 0x3fu) << 16) | (uint16_t)c->r[3]);",
                     "\treturn;"], 1, "done")

        if op1 == 1 and (op & 0xF3C0) == 0xF040:  # CALL16
            imm = img.word(pc + 1)
            if imm is None:
                note("fxxx-call-outside")
                return None
            ret = (pc + 2) & PC_MASK
            tgt = ((op & 0x3F) << 16) | imm
            return ([hook,
                     "\t{",
                     "\t\tconst uint16_t sr_ret = (uint16_t)((c->r[6] & 0xffc0u) | 0x%04Xu);"
                     % ((ret >> 16) & 0x3F),
                     "\t\taot_push(c, 0, 0x%04Xu);" % (ret & 0xFFFF),
                     "\t\taot_push(c, 0, sr_ret);",
                     "\t}",
                     "\tAOT_CYC(c, 0x%06Xu, 9);" % pc,
                     "\taot_set_pc(c, 0x%06Xu);" % tgt,
                     "\treturn;"], 2, "done")

        # fxxx group 5 "nothing" forms (unsp_fxxx.cpp execute_fxxx_101_group):
        # the ISA-2.0 override (unsp_12_device::execute_fxxx_101_group, emu/isa/
        # unsp_fxxx.cpp:299) charges 1 cycle and then has no case for these
        # opcodes, so they fall through to unimplemented_opcode, which charges
        # 2 more: 3 cycles total with no side effect other than logerror.  0xF165 alone is a quarter of
        # all fallbacks in a 20 s run, so it is worth folding in.
        if (op & 0xF1FF) in (0xF165, 0xF16D, 0xF175, 0xF17D):
            return ([hook,
                     "\tAOT_CYC(c, 0x%06Xu, 3);" % pc,
                     ], 1, None)

        # fxxx group 0: the DS accessors (unsp_fxxx.cpp execute_fxxx_000_group).
        # set_ds() only rewrites SR[10..15], so the six PC-bank bits and the four
        # condition flags survive untouched and the block can keep running: these
        # forms charge no cycles and do not end the block.
        if (op & 0xFFC0) == 0xFE00:  # ds = imm6
            return ([hook,
                     "\tAOT_CYC(c, 0x%06Xu, 0);" % pc,
                     "\tc->r[6] = (uint16_t)((c->r[6] & 0x03ffu) | ((0x%Xu & 0x3fu) << 10));" % imm6,
                     ], 1, None)

        if (op & 0xF1F8) == 0xF020:  # rx = ds
            if opb in (6, 7):
                note("fxxx-ds-read-into-pc")
                return None
            return ([hook,
                     "\tAOT_CYC(c, 0x%06Xu, 0);" % pc,
                     "\tc->r[%d] = (uint16_t)((c->r[6] >> 10) & 0x3fu);" % opb,
                     ], 1, None)

        if (op & 0xF1F8) == 0xF028:  # ds = rx
            return ([hook,
                     "\tAOT_CYC(c, 0x%06Xu, 0);" % pc,
                     "\tc->r[6] = (uint16_t)((c->r[6] & 0x03ffu) | (((uint16_t)c->r[%d] & 0x3fu) << 10));" % opb,
                     ], 1, None)

        # fxxx group 4: MUL ss (unsp_fxxx.cpp execute_fxxx_100_group, mask
        # 0xf1f8 == 0xf108).  12 cycles; a 16x16 signed product whose high half
        # lands in R4 and low half in R3.  0xF70C at 005F95/005F99 is the single
        # largest remaining fallback site (271k hits in a 20 s autoplay run).
        if (op & 0xF1F8) == 0xF108:
            return ([hook,
                     "\tAOT_CYC(c, 0x%06Xu, 12);" % pc,
                     "\t{ const uint32_t a = c->r[%d], b = c->r[%d];" % (opa, opb),
                     "\t  uint32_t lres = a * b;",
                     "\t  if (b & 0x8000u) lres -= a << 16;",
                     "\t  if (a & 0x8000u) lres -= b << 16;",
                     "\t  c->r[4] = lres >> 16; c->r[3] = (uint16_t)lres; }",
                     ], 1, None)

        note("fxxx-other")
        return None

    # --------------------------------------------------------------- jumps
    if opa == 7 and op1 < 2:
        if op0 <= 13:
            off = imm6 if op1 == 0 else -imm6
            tgt = (pc + 1 + off) & PC_MASK
            nxt = (pc + 1) & PC_MASK
            return ([hook,
                     "\tif (%s)" % COND[op0],
                     "\t{",
                     "\t\tAOT_CYC(c, 0x%06Xu, 4);" % pc,
                     "\t\taot_set_pc(c, 0x%06Xu);" % tgt,
                     "\t}",
                     "\telse",
                     "\t{",
                     "\t\tAOT_CYC(c, 0x%06Xu, 2);" % pc,
                     "\t\taot_set_pc(c, 0x%06Xu);" % nxt,
                     "\t}",
                     "\treturn;"], 1, "done")
        if op0 == 14:  # JMP
            off = imm6 if op1 == 0 else -imm6
            tgt = (pc + 1 + off) & PC_MASK
            return ([hook,
                     "\tAOT_CYC(c, 0x%06Xu, 4);" % pc,
                     "\taot_set_pc(c, 0x%06Xu);" % tgt,
                     "\treturn;"], 1, "done")
        note("jumps-op0-15")
        return None

    # ---------------------------------------------------------------- exxx
    if op0 == 0xE:
        # 16-bit shift through a register (unsp_exxx.cpp, unsp_12_device::
        # execute_exxx_group, mask 0xf188 == 0xe108).  None of the handled cases
        # touch m_icount, so they charge no cycles and do not end the block.
        # case 0/2/4 write back to rd, case 1/3/5 always target R3/R4.
        if (op & 0xF188) == 0xE108:
            sh = (op & 0x0070) >> 4
            rd = (op & 0x0E00) >> 9
            rs = op & 0x0007
            amount = "((uint16_t)c->r[%d] & 0x1fu)" % rs
            if sh in (0, 2, 4):
                if rd in (6, 7):
                    note("exxx-shift-into-pc")
                    return None
                if sh == 0:      # asr: the 32-bit result keeps the sign extension
                    body = ["\t{ const int32_t v = (int16_t)(uint16_t)c->r[%d];" % rd,
                            "\t  c->r[%d] = (uint32_t)(v >> %s); }" % (rd, amount)]
                elif sh == 2:    # lsl
                    body = ["\t{ const uint32_t v = (uint16_t)c->r[%d];" % rd,
                            "\t  c->r[%d] = (uint16_t)(v << %s); }" % (rd, amount)]
                else:            # lsr
                    body = ["\t{ const uint32_t v = (uint16_t)c->r[%d];" % rd,
                            "\t  c->r[%d] = (uint16_t)(v >> %s); }" % (rd, amount)]
            elif sh == 1:        # asror
                body = ["\t{ const int32_t v = (int32_t)((uint32_t)c->r[%d] << 16);" % rd,
                        "\t  const uint32_t res = (uint32_t)(v >> %s);" % amount,
                        "\t  c->r[3] |= (uint16_t)res; c->r[4] = (uint16_t)(res >> 16); }"]
            elif sh == 3:        # lslor
                body = ["\t{ const uint32_t res = (uint16_t)c->r[%d] << %s;" % (rd, amount),
                        "\t  c->r[3] = (uint16_t)res; c->r[4] |= (uint16_t)(res >> 16); }"]
            elif sh == 5:        # lsror
                body = ["\t{ const uint32_t res = ((uint32_t)c->r[%d] << 16) >> %s;" % (rd, amount),
                        "\t  c->r[3] |= (uint16_t)res; c->r[4] = (uint16_t)(res >> 16); }"]
            else:
                note("exxx-rol-ror")
                return None
            return ([hook, "\tAOT_CYC(c, 0x%06Xu, 0);" % pc] + body, 1, None)

        # MUL uu (unsp_exxx.cpp, mask 0xf1f8 == 0xe008): 32x32 -> low 32 into
        # R3/R4, 12 cycles.  The multiply reads the full 32-bit register values,
        # exactly like the interpreter, so a sign-extended value left by ASR
        # contributes the same way here.
        if (op & 0xF1F8) == 0xE008:
            return ([hook,
                     "\tAOT_CYC(c, 0x%06Xu, 12);" % pc,
                     "\t{ const uint32_t lres = c->r[%d] * c->r[%d];" % (opa, opb),
                     "\t  c->r[4] = lres >> 16; c->r[3] = (uint16_t)lres; }",
                     ], 1, None)

        note("exxx")
        return None

    lower = (op1 << 4) | op0

    # ---------------------------------------------------------- push / pop
    if lower == 0x2D:  # PUSH r(opa-opn+1..opa) -> [r(opb)]
        if opn > opa:
            note("push-underflow")
            return None
        if opb in (6, 7):  # stack pointer in SR/PC: not worth modelling
            note("push-stack-reg")
            return None
        n = 4 + 2 * opn
        lines = [hook, "\t{"]
        if opn and loc:
            # r0..r7 are separate C variables, so the interpreter's c->r[opa-i]
            # cannot stay an index expression: unroll the range instead.
            for k in range(opn):
                lines.append("\t\taot_push_sr(c, r%d, (uint16_t)r%d);" % (opb, opa - k))
        elif opn:
            lines.append("\t\tfor (unsigned i = 0; i < %du; i++)" % opn)
            lines.append("\t\t\taot_push(c, %du, (uint16_t)c->r[%d - i]);" % (opb, opa))
        lines.append(cyc_line.format(pc, n))
        lines.append("\t}")
        return (lines, 1, None)

    if lower == 0x29:  # POP [r(opb)] -> r(opa+1..opa+opn)
        if op == 0x9A98:  # RETI: needs m_ine / m_fiq / m_irq, left to the core
            note("reti")
            return None
        if opa + opn > 7:
            note("pop-overflow")
            return None
        if opb in (6, 7):
            note("pop-stack-reg")
            return None
        n = 4 + 2 * opn
        lines = [hook, "\t{"]
        if opn and loc:
            for k in range(1, opn + 1):
                lines.append("\t\tr%d = aot_pop_sr(c, r%d);" % (opa + k, opb))
        elif opn:
            lines.append("\t\tfor (unsigned i = 1; i <= %du; i++)" % opn)
            lines.append("\t\t\tc->r[%d + i] = aot_pop(c, %du);" % (opa, opb))
        lines.append(cyc_line.format(pc, n))
        lines.append("\t}")
        if opa + opn == 7:
            return (lines, 1, ("end", "pc"))
        if opa + opn == 6:
            return (lines, 1, ("end", "sr"))
        return (lines, 1, None)

    # ----------------------------------------------------------- ALU group
    if op0 not in ALU_OP0:
        note("alu-op0-%x" % op0)
        return None

    fl = "true" if opa != 7 else "false"
    lines = [hook, "\t{",
             "\t\tuint16_t r0v = (uint16_t)c->r[%d];" % opa,
             "\t\tuint16_t r1v = 0;",
             "\t\tuint32_t r2v = 0;"]
    length = 1
    n = 0
    extra_end = False

    if op1 == 0x00:  # r, [bp+imm6]
        n = 6
        lines.append("\t\tr2v = (uint16_t)(c->r[5] + %du);" % imm6)
        if op0 != 0x0D:
            lines.append("\t\tr1v = aot_rd(c, r2v);")

    elif op1 == 0x01:  # r, imm6
        n = 2
        lines.append("\t\tr1v = 0x%04Xu;" % imm6)

    elif op1 == 0x02:  # not decoded by the interpreter: r1 stays 0, no charge
        n = 0
        note("alu-op1-2")

    elif op1 == 0x03:  # indirect
        if (opn & 3) != 0 and opb in (6, 7):
            note("indirect-postinc-reg")
            return None
        n = 7 if opa == 7 else 6
        ds = (opn & 4) != 0
        address = ds_expr(opb) if ds else "c->r[%d]" % opb
        lsb = opn & 3
        if lsb == 0:
            lines.append("\t\tr2v = %s;" % address)
            if op0 != 0x0D:
                lines.append("\t\tr1v = aot_rd(c, r2v);")
        elif lsb == 1:
            lines.append("\t\tr2v = %s;" % address)
            if op0 != 0x0D:
                lines.append("\t\tr1v = aot_rd(c, r2v);")
            lines.append("\t\tc->r[%d] = (uint16_t)(c->r[%d] - 1u);" % (opb, opb))
            if ds:
                lines.append("\t\tif (c->r[%d] == 0xffffu) c->r[6] -= 0x0400u;" % opb)
        elif lsb == 2:
            lines.append("\t\tr2v = %s;" % address)
            if op0 != 0x0D:
                lines.append("\t\tr1v = aot_rd(c, r2v);")
            lines.append("\t\tc->r[%d] = (uint16_t)(c->r[%d] + 1u);" % (opb, opb))
            if ds:
                lines.append("\t\tif (c->r[%d] == 0u) c->r[6] += 0x0400u;" % opb)
        else:
            lines.append("\t\tc->r[%d] = (uint16_t)(c->r[%d] + 1u);" % (opb, opb))
            if ds:
                lines.append("\t\tif (c->r[%d] == 0u) c->r[6] += 0x0400u;" % opb)
            lines.append("\t\tr2v = %s;" % address)
            if op0 != 0x0D:
                lines.append("\t\tr1v = aot_rd(c, r2v);")
        if lsb != 0 and opb in (6, 7):
            extra_end = True
    elif op1 == 0x04:
        if opn == 0x00:  # r, r
            n = 5 if opa == 7 else 3
            lines.append("\t\tr1v = (uint16_t)c->r[%d];" % opb)
        elif opn == 0x01:  # r, imm16
            imm = img.word(pc + 1)
            if imm is None:
                note("imm16-outside")
                return None
            length = 2
            n = 5 if opa == 7 else 4
            lines.append("\t\tr0v = (uint16_t)c->r[%d];" % opb)
            lines.append("\t\tr1v = 0x%04Xu;" % imm)
        elif opn == 0x02:  # r, [imm16]
            imm = img.word(pc + 1)
            if imm is None:
                note("imm16-outside")
                return None
            length = 2
            n = 8 if opa == 7 else 7
            lines.append("\t\tr0v = (uint16_t)c->r[%d];" % opb)
            lines.append("\t\tr2v = 0x%04Xu;" % imm)
            if op0 != 0x0D:
                lines.append("\t\tr1v = aot_rd(c, r2v);")
        elif opn == 0x03:  # [imm16], r
            imm = img.word(pc + 1)
            if imm is None:
                note("imm16-outside")
                return None
            length = 2
            n = 8 if opa == 7 else 7
            lines.append("\t\tr1v = r0v;")
            lines.append("\t\tr0v = (uint16_t)c->r[%d];" % opb)
            lines.append("\t\tr2v = 0x%04Xu;" % imm)
        else:  # shifted ops
            n = 5 if opa == 7 else 3
            lines.append("\t\t{ uint32_t sh = (((uint32_t)(uint16_t)c->r[%d] << 4) "
                         "| (c->sb & 0xfu));" % opb)
            lines.append("\t\t  if (sh & 0x80000u) sh |= 0xf00000u;")
            lines.append("\t\t  sh >>= %du;" % (opn - 3))
            lines.append("\t\t  c->sb = sh & 0xfu;")
            lines.append("\t\t  r1v = (uint16_t)(sh >> 4); }")

    elif op1 == 0x05:  # shift left/right through the shift buffer
        n = 5 if opa == 7 else 3
        if opn & 4:
            lines.append("\t\t{ uint32_t sh = ((((uint32_t)(uint16_t)c->r[%d] << 4) "
                         "| (c->sb & 0xfu)) >> %du);" % (opb, opn - 3))
            lines.append("\t\t  c->sb = sh & 0xfu;")
            lines.append("\t\t  r1v = (uint16_t)(sh >> 4); }")
        else:
            lines.append("\t\t{ uint32_t sh = (((c->sb & 0xfu) << 16) "
                         "| (uint16_t)c->r[%d]) << %du;" % (opb, opn + 1))
            lines.append("\t\t  c->sb = (sh >> 16) & 0xfu;")
            lines.append("\t\t  r1v = (uint16_t)sh; }")

    elif op1 == 0x06:  # rotate
        n = 5 if opa == 7 else 3
        lines.append("\t\t{ uint32_t sh = ((((c->sb & 0xfu) << 16) "
                     "| (uint16_t)c->r[%d]) << 4) | (c->sb & 0xfu);" % opb)
        if opn & 4:
            lines.append("\t\t  sh >>= %du;" % (opn - 3))
            lines.append("\t\t  c->sb = sh & 0xfu;")
        else:
            lines.append("\t\t  sh <<= %du;" % (opn + 1))
            lines.append("\t\t  c->sb = (sh >> 20) & 0xfu;")
        lines.append("\t\t  r1v = (uint16_t)(sh >> 4); }")

    else:  # op1 == 0x07, direct 6-bit address
        n = 6 if opa == 7 else 5
        lines.append("\t\tr2v = 0x%04Xu;" % imm6)
        lines.append("\t\tr1v = aot_rd(c, r2v);")

    # Write-back, exactly as execute_remaining() + do_basic_alu_ops() do it.
    if op0 == 0x0D:  # store
        lines.append("\t\taot_wr(c, r2v, r0v);")
    elif op0 in (0x04, 0x0C):  # compare / test: flags only
        lines.append("\t\taot_op(c, 0x%x, r0v, r1v, %s);" % (op0, fl))
    elif op1 == 0x04 and opn == 0x03:
        lines.append("\t\t{ const uint32_t lr = aot_op(c, 0x%x, r0v, r1v, %s);" % (op0, fl))
        lines.append("\t\t  if (lr != AOT_NO_WRITE) aot_wr(c, r2v, (uint16_t)lr); }")
    else:
        lines.append("\t\t{ const uint32_t lr = aot_op(c, 0x%x, r0v, r1v, %s);" % (op0, fl))
        lines.append("\t\t  if (lr != AOT_NO_WRITE) c->r[%d] = (uint16_t)lr; }" % opa)
    lines.append(cyc_line.format(pc, n))
    lines.append("\t}")

    writes_dest = op0 not in NO_WRITE_OP0 and not (op1 == 0x04 and opn == 0x03)
    if writes_dest and opa in (6, 7):
        return (lines, length, ("end", "pc" if opa == 7 else "sr"))
    if extra_end:
        return (lines, length, ("end", "sr"))
    return (lines, length, None)


def emit_body(img, entry, loc=False):
    """Translate one block.  Returns (lines, native_count, worst_cycles).

    Every instruction charges its own cycles through AOT_CYC and then checks the
    two exit conditions in the same order the interpreter's run loop does:
    a pending interrupt first, then a negative icount.  Charging the whole block
    at the end instead would let the block overshoot the slice budget by several
    instructions, which desynchronises the slice boundaries (and therefore the
    interrupt landings) from interpreter-only runs.

    With loc=True the body runs on block-local r0..r7/sb: AOT_LOAD_REGS on entry,
    one AOT_SAVE_REGS per exit.  callers pick that form by block length, because
    the save/restore pair costs more than it saves on a one-instruction block.
    """
    lines = []
    if loc:
        # The declaration has to live in the function scope, not inside the
        # load macro: the macro only assigns, so it cannot introduce names that
        # the rest of the block goes on to use.
        lines.append("\tuint32_t r0, r1, r2, r3, r4, r5, r6, r7, sb;")
        lines.append("\tAOT_LOAD_REGS(c);")
    pc = entry
    native = 0
    worst = 0
    pcs = []
    for index in range(MAX_BLOCK_INSNS):
        pcs.append(pc)
        insn = emit(img, pc, index == 0, loc)
        if insn is None:
            lines.append("\t%s(c, 0x%06Xu);" % ("AOT_I" if index == 0 else "AOT_IH", pc))
            if loc:
                lines.append("\tAOT_SAVE_REGS(c);")
            lines.append("\tc->fallback(c->dev, c, 0x%06Xu);" % pc)
            lines.append("\treturn;")
            return lines, native, worst, pcs
        ilines, length, term = insn
        lines.extend(ilines)
        native += 1
        settled = [int(m) for m in re.findall(r"AOT_CYC\(c, 0x[0-9A-Fa-f]+u, (\d+)\)", "\n".join(ilines))]
        worst += max(settled) if settled else 0
        nxt = (pc + length) & PC_MASK
        if term == "done":
            return lines, native, worst, pcs
        if term is not None:
            # The block stops here because the instruction touched SR or PC.  A
            # block keeps r7 (the PC low half) parked on the block entry, so
            # writing SR alone leaves it stale: publish the fall-through PC.
            if term[1] == "sr":
                if loc:
                    lines.append("\tr7 = 0x%04Xu;" % (nxt & 0xFFFF))
                else:
                    lines.append("\tc->r[7] = 0x%04Xu;" % (nxt & 0xFFFF))
            if loc:
                lines.append("\tAOT_SAVE_REGS(c);")
            lines.append("\treturn;")
            return lines, native, worst, pcs
        # Leaving the block half way must publish the PC the next instruction
        # lives at: a block does not maintain r7 as it runs.  The interrupt test
        # comes first because the interpreter's run loop takes a pending IRQ
        # right after the instruction, before it looks at the cycle budget.
        if not LOOSE:
            save = "AOT_SAVE_REGS(c); " if loc else ""
            lines.append("\tif (*c->sirq) { %saot_set_pc(c, 0x%06Xu); return; }" % (save, nxt))
            lines.append("\tif (c->icount < 0) { %saot_set_pc(c, 0x%06Xu); return; }" % (save, nxt))
        pc = nxt

    lines.append("\tAOT_IH(c, 0x%06Xu);" % pc)
    if loc:
        lines.append("\tAOT_SAVE_REGS(c);")
    lines.append("\tc->fallback(c->dev, c, 0x%06Xu);" % pc)
    lines.append("\treturn;")
    return lines, native, worst, pcs


def finish_block(lines):
    """In loose mode, charge the block's cycles once, on every way out.

    A block does not maintain the cycle count as it runs, so every exit (natural
    end, taken branch, untranslatable instruction) has to add the accumulator to
    the budget.  PC builds charge per instruction instead and skip this.
    """
    if not LOOSE:
        return lines
    out = []
    for ln in lines:
        m = _AOT_CYC_RE.match(ln)
        if m:
            # Same charge, but into the block-local accumulator: no DRAM store
            # per instruction, one subtraction on the way out.
            out.append(m.group(1) + "cyc += " + m.group(2) + ";")
            continue
        if ln.strip() == "return;":
            indent = ln[:len(ln) - len(ln.lstrip())]
            out.append(indent + "c->icount -= cyc;")
        out.append(ln)
    return out


_SET_PC_RE = re.compile(r"aot_set_pc\(c, 0x([0-9A-Fa-f]{6})u\);")
_SET_PC_ANY_RE = re.compile(r"aot_set_pc\(c, ")


def exit_targets(lines):
    """Literal targets reachable from the block tail, as ints.

    The per-instruction guards (`if (*c->sirq) { ...; return; }`) also publish a
    PC, but they return straight to the dispatcher instead of falling into the
    next block, so they are not group edges.
    """
    out = []
    for ln in lines:
        if " return; }" in ln:
            continue
        for m in _SET_PC_RE.finditer(ln):
            out.append(int(m.group(1), 16))
    return out


def groupable(lines):
    """True when every way out of the block names its successor literally.

    Blocks that leave on a computed PC (CALL16 / JMPF / JMPR), on an SR write
    (the PC high half is only known at run time) or on the one-instruction
    fallback cannot be chained: their successor set is not known statically, so
    the group would lose its single-entry property.
    """
    if not exit_targets(lines):
        return False
    for ln in lines:
        if ln.strip().startswith("c->fallback("):
            return False
        if re.match(r"^\s*(r7|c->r\[7\]) = 0x[0-9A-Fa-f]{4}u;\s*$", ln):
            return False
        if _SET_PC_ANY_RE.search(ln) and not _SET_PC_RE.search(ln):
            return False
    return True


def groupify(members, loc):
    """Rewrite a group's blocks into one function body plus its exit labels."""
    # Only members that somebody actually jumps to need a label; labelling the
    # leader unconditionally provokes -Wunused-label on acyclic groups.
    mset = set(m[0] for m in members)
    need = set()
    for entry, native, lines, worst in members:
        for ln in lines:
            m = _SET_PC_RE.search(ln)
            if m and " return; }" not in ln:
                t = int(m.group(1), 16)
                if t in mset:
                    need.add(t)
    body = []
    targets = []
    for entry, native, lines, worst in members:
        if entry in need:
            body.append("L_%06X:" % entry)
        for ln in lines:
            s = ln.strip()
            if s.startswith("uint32_t r0, r1,") or s == "AOT_LOAD_REGS(c);":
                continue
            if s == "c->icount -= cyc;" or s == "return;":
                continue
            m = _SET_PC_RE.search(ln)
            if m and " return; }" not in ln:
                tgt = int(m.group(1), 16)
                targets.append(tgt)
                ind = ln[:len(ln) - len(ln.lstrip())]
                if loc:
                    body.append("%sr6 = (r6 & 0xffc0u) | 0x%02Xu; r7 = 0x%04Xu; goto LAB_%06X;"
                                % (ind, (tgt >> 16) & 0x3F, tgt & 0xFFFF, tgt))
                else:
                    body.append("%saot_set_pc(c, 0x%06Xu); goto LAB_%06X;" % (ind, tgt, tgt))
                continue
            body.append(ln)
    return body, targets


def build_groups(bodies):
    """Merge blocks into single-entry superblocks, longest block first.

    A block may join a group only when every predecessor the generator can name
    is already in that group: that is exactly the condition that keeps the
    group's first block as its sole entry point.  Code that reaches the middle of
    a group anyway (a computed jump) finds a null table slot and drops to the
    interpreter, so a wrong static guess costs speed, never correctness.
    """
    info = dict((entry, (native, worst)) for entry, native, lines, worst in bodies)
    body_of = dict((entry, lines) for entry, native, lines, worst in bodies)
    # Mixing the two register forms inside one function is not expressible: a
    # localised block reads r0..r7 and an unlocalised one reads c->r[].  Only
    # chain blocks that agree.
    loc_of = dict((entry, lines[0].strip().startswith("uint32_t r0,"))
                  for entry, native, lines, worst in bodies)
    succ = dict((entry, exit_targets(lines)) for entry, native, lines, worst in bodies)
    pred = {}
    for entry, succs in succ.items():
        for t in succs:
            pred.setdefault(t, set()).add(entry)

    order = sorted(bodies, key=lambda b: (-b[1], b[0]))
    assigned = set()
    groups = []
    for entry, native, lines, worst in order:
        if entry in assigned or not groupable(lines) or not succ[entry]:
            continue
        members = [entry]
        gset = {entry}
        cur = native
        assigned.add(entry)
        progress = True
        while progress and len(members) < MAX_GROUP_BLOCKS:
            progress = False
            for n in list(members):
                for t in succ[n]:
                    if t in gset or t not in info or t in assigned:
                        continue
                    if len(members) >= MAX_GROUP_BLOCKS:
                        break
                    if cur + info[t][0] > MAX_GROUP_INSNS:
                        continue
                    if not groupable(body_of[t]):
                        continue
                    if loc_of[t] != loc_of[entry]:
                        continue
                    if any(p not in gset and p != t for p in pred.get(t, ())):
                        continue
                    members.append(t)
                    gset.add(t)
                    cur += info[t][0]
                    assigned.add(t)
                    progress = True
        if len(members) >= 2:
            groups.append((entry, [(e, info[e][0], body_of[e], info[e][1]) for e in members]))
    return groups


def main():
    global _NOTE_ON
    blocks = []
    with open(BLOCKS, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            blocks.append(int(line.split()[0].rstrip(','), 16))
    extra = []
    if EXTRA and os.path.exists(EXTRA):
        with open(EXTRA, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                extra.append(int(line.split()[0], 16))
        extra.sort()

    img = Images(FLASH, IROM)

    bodies = []
    pages = {}
    native_total = 0
    registered = 0
    truncated = 0
    localised = 0
    insn_total = 0
    covered = set()
    # Real block entries first, then any remaining hot PC that no block already
    # translated.  Emitting a block per executed PC without this pass would
    # translate the same instructions once per overlapping entry (23 MB of C for
    # 762 + 7,437 entries); the greedy sweep keeps every instruction once.
    force = set()
    if FORCE and os.path.exists(FORCE):
        with open(FORCE, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                force.add(int(line.split()[0], 16))

    todo = [(entry, True) for entry in blocks]
    todo += [(entry, False) for entry in extra if entry not in force]
    todo += [(entry, False) for entry in sorted(force)]
    emitted = set()
    for entry, is_entry in todo:
        # One C function per entry: a PC can appear in more than one source
        # list (a static block entry that is also in AOT_FORCE, say), and
        # emitting it twice is a redefinition error.
        if entry in emitted:
            continue
        if not is_entry and entry in covered and entry not in force:
            continue
        emitted.add(entry)
        lines, native, worst, pcs = emit_body(img, entry)
        if LOOSE and native >= LOCAL_MIN_INSNS:
            # Second pass with r0..r7 held in C locals.  Only blocks long enough
            # for the save/restore pair to amortise take it: measured dispatch is
            # 68 cycles and a short block cannot pay that back twice.
            #
            # PC builds stay on the c->r[] form on purpose.  They check the
            # interrupt and cycle budget after *every* instruction, so every one
            # of those exits would need a full 10-register flush (12444 of them
            # across the table) -- which costs more than the localisation saves,
            # and the unmodified form is what the four-window trace comparison
            # is calibrated against.  The device build is LOOSE, exits once per
            # block, and is the one that needs the speed.
            _NOTE_ON = False
            l2, n2, w2, p2 = emit_body(img, entry, True)
            _NOTE_ON = True
            if n2 == native and p2 == pcs:
                lines, worst = l2, w2
                localised += 1
            else:
                # A translation that terminates elsewhere is a bug in the
                # rewriting pass; keep the audited form and say so loudly.
                print("LOCALISATION MISMATCH at %06X: %d/%d insns, tails %s/%s"
                      % (entry, native, n2, pcs[-1:], p2[-1:]))
        lines = finish_block(lines)
        insn_total += native
        # A static block entry is a real function in the original code, so it is
        # always worth a table slot.  A hot PC is only worth one when the block
        # is long enough to amortise the ~30 cycles of dispatch: a one or two
        # instruction block costs *more* than letting the interpreter do it
        # (measured: adding the short JMPF entries at 0x207A57/0x207A73 dropped
        # the mean frame rate from 36.9 to 34.3 fps).
        if not is_entry and native < MIN_NATIVE_INSNS:
            truncated += 1
            continue
        covered.update(pcs)
        bodies.append((entry, native, lines, worst))
        pages.setdefault((entry >> 8) & 0x3FFF, []).append((entry & 0xFF, entry, worst))
        native_total += native
        registered += 1
    groups = []
    grouped = set()
    if GROUP:
        groups = build_groups(bodies)
        for leader, members in groups:
            for e, _n, _l, _w in members:
                grouped.add(e)

    skipped_overlap = sum(1 for entry, is_entry in todo
                          if not is_entry and entry in covered and entry not in force)

    out = []
    out.append("// AUTO-GENERATED by tools/aot.py -- do not edit by hand.")
    out.append("// Source: tools/coverage/out/AB_blocks.txt, roms/*.bin")
    out.append("//")
    out.append("// %d block entries + %d hot PCs, %d registered, %d native instructions."
               % (len(blocks), len(extra), registered, native_total))
    out.append("#include \"aot.h\"")
    out.append("#include <cstring>")
    out.append("#ifdef ESP_PLATFORM")
    out.append("#include \"esp_attr.h\"")
    out.append("#define AOT_IRAM IRAM_ATTR")
    # Lookup tables for the hot pages: the block body being in IRAM only removes
    # the fetch; aot_lookup() still reads fn[slot] and cnt[slot] from flash on
    # every dispatch, which on the C3 costs more than the block itself.
    # .dram0.data, not .iram0.*: the C3 linker script only accepts executable
    # flags in .iram0.text, so a const array placed there becomes an orphan and
    # gets appended to DRAM (which then overflows).  DRAM is a 3-cycle load and
    # sits next to the block tables anyway - that is fast enough.
    out.append("#define AOT_IRAM_DATA __attribute__((section(\".dram0.data\")))")
    out.append("#else")
    out.append("#define AOT_IRAM")
    out.append("#define AOT_IRAM_DATA")
    out.append("#endif")
    out.append("")
    for entry, native, lines, worst in bodies:
        if entry in grouped:
            continue
        out.append("// %06X: %d native instructions, %d cycles worst case" % (entry, native, worst))
        out.append(("AOT_IRAM " if entry in IRAM_SET else "") + "static void aot_b%06X(AotCtx *c)" % entry)
        out.append("{")
        if LOOSE:
            out.append("\tint32_t cyc = 0;")
        out.extend(lines)
        out.append("}")
        out.append("")

    # A group is one function with a label per member and a label per distinct
    # exit target.  Register file loads happen once on entry; the flush, the
    # cycle charge and the interrupt test happen once per boundary - the same
    # points where the ungrouped build returned to the dispatcher.
    for leader, members in groups:
        member_set = set(m[0] for m in members)
        loc = members[0][2][0].strip().startswith("uint32_t r0,")
        body, targets = groupify(members, loc)
        out.append("// superblock %06X: %d blocks, %d native instructions"
                   % (leader, len(members), sum(m[1] for m in members)))
        out.append("//   members: " + " ".join("%06X" % m[0] for m in members))
        out.append(("AOT_IRAM " if leader in IRAM_SET else "")
                   + "static void aot_g%06X(AotCtx *c)" % leader)
        out.append("{")
        if LOOSE:
            out.append("\tint32_t cyc = 0;")
        if loc:
            out.append("\tuint32_t r0, r1, r2, r3, r4, r5, r6, r7, sb;")
            out.append("\tAOT_LOAD_REGS(c);")
        out.extend(body)
        for tgt in sorted(set(targets)):
            out.append("LAB_%06X:" % tgt)
            if LOOSE:
                out.append("\tc->icount -= cyc; cyc = 0;")
            out.append("\tif (c->icount < 0 || *c->sirq)")
            out.append("\t{")
            if loc:
                out.append("\t\tAOT_SAVE_REGS(c);")
            out.append("\t\treturn;")
            out.append("\t}")
            if tgt in member_set:
                out.append("\tgoto L_%06X;" % tgt)
            else:
                # Leaves the group: the PC is already published, so hand the
                # block back to the dispatcher through the normal return path.
                if loc:
                    out.append("\tAOT_SAVE_REGS(c);")
                out.append("\treturn;")
        out.append("}")
        out.append("")

    # Table slots.  A group's first block owns the entry point and the summed
    # count/cycles; the other members get a null slot, so a computed jump into
    # the middle of a group drops to the interpreter instead of running the tail
    # of a function whose prologue never ran.
    fn_of = dict((entry, "aot_b%06X" % entry) for entry, native, lines, worst in bodies)
    info = dict((entry, (native, worst)) for entry, native, lines, worst in bodies)
    for leader, members in groups:
        fn_of[leader] = "aot_g%06X" % leader
        info[leader] = (sum(m[1] for m in members), sum(m[3] for m in members))
        for e, _n, _l, _w in members[1:]:
            fn_of[e] = "nullptr"
            info[e] = (0, 0)

    if CONST_TABLES:
        # Device build: everything below is `const`, so it is placed in .rodata
        # (flash on the C3) instead of costing RAM.  Full 256-entry arrays are
        # written out because minGW rejects designated initialisers here; the
        # unused slots are plain nullptr / 0.
        for page in sorted(pages):
            # One 8-byte struct per block slot so fn/cyc/cnt share a cache line;
            # three parallel arrays cost three flash reads per dispatch.
            ent = ["{ nullptr, 0, 0, 0 }"] * 0x100
            for slot, entry, worst in pages[page]:
                native, cyc = info[entry]
                ent[slot] = "{ %s, %d, %d, 0 }" % (fn_of[entry], cyc, native)
            out.append("static const AotEnt aot_ent_%03X[0x100] = {" % page)
            for i in range(0, 0x100, 4):
                out.append("\t" + ", ".join(ent[i:i + 4]) + ",")
            out.append("};")
        out.append("")
        out.append("const AotPageDesc g_aot_desc[] = {")
        for page in sorted(pages):
            out.append("\t{ 0x%03X, aot_ent_%03X },"
                       % (page, page))
        out.append("};")
        out.append("const uint32_t g_aot_desc_n = %du;" % len(pages))
        out.append("")
    else:
        for page in sorted(pages):
            out.append("static AotPage aot_pg_%03X;" % page)
        out.append("")
        out.append("AotPage *g_aot_page[0x4000];")
        out.append("")
        out.append("// Page tables are filled once at start-up: minGW rejects the")
        out.append("// designated-initialiser form for an array of function pointers.")
        out.append("void aot_init(void)")
        out.append("{")
        for page in sorted(pages):
            out.append("\tstd::memset(aot_pg_%03X.fn, 0, sizeof(aot_pg_%03X.fn));" % (page, page))
        for page in sorted(pages):
            for slot, entry, worst in sorted(pages[page]):
                out.append("\taot_pg_%03X.fn[0x%02X] = %s;" % (page, slot, fn_of[entry]))
        for entry, native, lines, worst in bodies:
            cnt, cyc = info[entry]
            out.append("\taot_pg_%03X.cnt[0x%02X] = %d;"
                       % ((entry >> 8) & 0x3FFF, entry & 0xFF, cnt))
            out.append("\taot_pg_%03X.cyc[0x%02X] = %d;"
                       % ((entry >> 8) & 0x3FFF, entry & 0xFF, cyc))
        for page in sorted(pages):
            out.append("\tg_aot_page[0x%03X] = &aot_pg_%03X;" % (page, page))
        out.append("}")
        out.append("")
        out.append("namespace { struct AotInit { AotInit() { aot_init(); } } g_aot_init; }")
        out.append("")

    out.append("const uint32_t g_aot_block_count = %du;" % registered)
    out.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))

    print("blocks read      : %d" % len(blocks))
    print("registered       : %d" % registered)
    print("hot PCs covered  : %d / %d" % (len(extra) - skipped_overlap, len(extra)))
    print("skipped (<%d ins) : %d" % (MIN_NATIVE_INSNS, truncated))
    print("native insns     : %d" % native_total)
    print("localised blocks : %d (>= %d insns)" % (localised, LOCAL_MIN_INSNS))
    print("avg native/block : %.2f" % (native_total / registered if registered else 0.0))
    print("pages            : %d" % len(pages))
    if GROUP:
        print("superblocks      : %d, %d blocks chained (%.2f blocks/group)"
              % (len(groups), len(grouped),
                 len(grouped) / float(len(groups)) if groups else 0.0))
    if REASONS:
        print("truncation reasons (instruction instances, not blocks):")
        for reason, count in sorted(REASONS.items(), key=lambda kv: -kv[1]):
            print("  %-22s %d" % (reason, count))
    print("wrote %s (%d bytes)" % (OUT, os.path.getsize(OUT)))


if __name__ == "__main__":
    main()
