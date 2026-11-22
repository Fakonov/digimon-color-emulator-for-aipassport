#!/usr/bin/env python3
"""Static unSP decoder: instruction length + control-flow class.

Rules taken from MAME 0.289 src/devices/cpu/unsp/*.cpp (copies in ref/unsp/):

  execute_one()          op0==0xf -> fxxx group, (opA==7 && op1<2) -> jumps,
                         op0==0xe -> exxx group, else execute_remaining()
  execute_jumps()        op0 0..13 conditional imm6, op0==14 JMP imm6
                         target = lpc + (op1==0 ? +imm6 : -imm6), lpc == pc+1
  unsp_fxxx.cpp          sub-group index is op1:
                           op1==0 fe00/f020/f028/f030/f038/MUL us   len 1
                           op1==1 CALL16 0xf040 or bitop +imm16     len 2
                           op1==2 JMPF 0xfe80 only                  len 2
                           op1==3 JMPR 0xfec0 (PC = r3)             len 1
                           op1==4/5 MUL ss / divq                   len 1
                           op1==6 MULS with ximm                    len 2
                           op1==7 MULS size 8..15                   len 1
  unsp_other.cpp         op1==4 with opn 1/2/3 -> imm16 / [imm16] / store [imm16]
                         -> len 2, everything else len 1
  push/pop               lower=(op1<<4)|op0; 0x2d push, 0x29 pop, reti 0x9a98
                         pop writes registers opa+1 .. opa+opn, so a pop that
                         covers register 7 also writes PC (return).
"""
import struct

KIND_SEQ = "seq"
KIND_COND = "cond"
KIND_JMP = "jmp"
KIND_CALL = "call"
KIND_IND = "ind_r3"
KIND_RET = "ret"
KIND_RETI = "reti"
KIND_INVALID = "invalid"

TRANSFERS = {KIND_COND, KIND_JMP, KIND_CALL, KIND_IND, KIND_RET, KIND_RETI}


class Images:
    def __init__(self, flash_path, irom_path):
        with open(flash_path, "rb") as f:
            self.flash = f.read()
        with open(irom_path, "rb") as f:
            self.irom = f.read()

    def region(self, pc):
        if 0x200000 <= pc < 0x400000:
            return "extflash"
        if 0x4000 <= pc < 0x10000:
            return "irom"
        if 0 < pc < 0x1000:
            return "ram"
        if 0x3000 <= pc < 0x3100:
            return "io"
        return "other"

    def word(self, pc):
        """Read a 16-bit little-endian instruction word, or None."""
        r = self.region(pc)
        if r == "extflash":
            off = (pc - 0x200000) * 2
            b = self.flash
        elif r == "irom":
            off = (pc - 0x4000) * 2
            b = self.irom
        else:
            return None
        if off + 1 >= len(b):
            return None
        return b[off] | (b[off + 1] << 8)


def decode(img, pc):
    """Return (length_words, kind, target_or_None, opcode)."""
    op = img.word(pc)
    if op is None:
        return 1, KIND_SEQ, None, None
    if op == 0 or op == 0xFFFF:
        return 1, KIND_INVALID, None, op

    op0 = (op >> 12) & 0xF
    opa = (op >> 9) & 7
    op1 = (op >> 6) & 7
    opn = (op >> 3) & 7

    if op0 == 0xF:
        if op1 == 0:
            return 1, KIND_SEQ, None, op
        if op1 == 1:
            if (op & 0xF3C0) == 0xF040:  # CALL16
                return 2, KIND_CALL, img.word(pc + 1), op
            return 2, KIND_SEQ, None, op  # bitop with imm16
        if op1 == 2:
            if (op & 0xFFC0) == 0xFE80:  # JMPF
                return 2, KIND_JMP, img.word(pc + 1), op
            return 1, KIND_SEQ, None, op
        if op1 == 3:
            if (op & 0xFFC0) == 0xFEC0:  # JMPR, PC = r3
                return 1, KIND_IND, None, op
            return 1, KIND_SEQ, None, op
        if op1 == 6:
            return 2, KIND_SEQ, None, op  # MULS with ximm
        return 1, KIND_SEQ, None, op

    if opa == 7 and op1 < 2:  # jumps: imm6, direction by op1
        imm = op & 0x3F
        tgt = (pc + 1 + (imm if op1 == 0 else -imm)) & 0x3FFFFF
        if op0 == 0xE:
            return 1, KIND_JMP, tgt, op
        return 1, KIND_COND, tgt, op

    if op0 == 0xE:
        return 1, KIND_SEQ, None, op  # exxx: int / irq / break etc.

    lower = (op1 << 4) | op0
    if lower == 0x2D:
        return 1, KIND_SEQ, None, op  # push
    if lower == 0x29:
        if op == 0x9A98:
            return 1, KIND_RETI, None, op
        if opa + opn >= 7:
            return 1, KIND_RET, None, op  # pop ... pc
        return 1, KIND_SEQ, None, op

    if op1 == 4 and opn in (1, 2, 3):
        return 2, KIND_SEQ, None, op  # imm16 / [imm16] / store [imm16]
    return 1, KIND_SEQ, None, op
