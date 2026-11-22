#ifndef UNSP_STATE_H
#define UNSP_STATE_H

#include <stdint.h>
#include <string.h>

/* unSP register file: r0=SP, r1..r4 general, r5=BP, r6=SR, r7=PC.
   The array is 32-bit because PC is a 24-bit word address. */
typedef struct {
    uint32_t r[8];
    uint16_t sb;              /* 4-bit shift buffer, stored in low nibble */
    uint16_t ram[0x1000];     /* 4K words: 0x0000..0x0FFF */
} State;

#define UNSP_N 0x0200u
#define UNSP_Z 0x0100u
#define UNSP_S 0x0080u
#define UNSP_C 0x0040u

static inline uint16_t unsp_ds(const State *s) {
    return (uint16_t)((s->r[6] >> 10) & 0x3f);
}

static inline void unsp_set_ds(State *s, uint16_t ds) {
    s->r[6] = (uint16_t)((s->r[6] & 0x03ffu) | ((ds & 0x3fu) << 10));
}

static inline uint16_t unsp_read16(State *s, uint32_t addr) {
    /* First unit: all observed addresses are RAM (DS=0).  Masking keeps the
       generated block branch-free; a full memory map replaces this later. */
    return s->ram[addr & 0x0fffu];
}

static inline void unsp_write16(State *s, uint32_t addr, uint16_t value) {
    s->ram[addr & 0x0fffu] = value;
}

static inline void unsp_update_nzsc(State *s, uint32_t value, uint16_t a, uint16_t b) {
    uint32_t f = s->r[6] & (uint16_t)~(UNSP_N | UNSP_Z | UNSP_S | UNSP_C);
    f |= (((value >> 16) ^ ((a ^ b) >> 15)) & 1u) << 7; /* S */
    f |= ((value >> 15) & 1u) << 9;                      /* N */
    f |= (uint32_t)((uint16_t)value == 0) << 8;          /* Z */
    f |= ((value >> 16) & 1u) << 6;                      /* C */
    s->r[6] = f;
}

static inline void unsp_update_nz(State *s, uint32_t value) {
    uint32_t f = s->r[6] & (uint16_t)~(UNSP_N | UNSP_Z);
    f |= ((value >> 15) & 1u) << 9;             /* N */
    f |= (uint32_t)((uint16_t)value == 0) << 8; /* Z */
    s->r[6] = f;
}

static inline void op_push(State *s, int reg) {
    unsp_write16(s, s->r[0] & 0xffffu, (uint16_t)s->r[reg]);
    s->r[0] = (uint16_t)(s->r[0] - 1);
}

static inline void op_add_imm6(State *s, int rd, uint16_t imm) {
    uint32_t value = (uint32_t)(uint16_t)s->r[rd] + imm;
    unsp_update_nzsc(s, value, (uint16_t)s->r[rd], imm);
    s->r[rd] = (uint16_t)value;
}

static inline void op_sub_imm6(State *s, int rd, uint16_t imm) {
    uint16_t n = (uint16_t)~imm;
    uint32_t value = (uint32_t)(uint16_t)s->r[rd] + n + 1u;
    unsp_update_nzsc(s, value, (uint16_t)s->r[rd], n);
    s->r[rd] = (uint16_t)value;
}

static inline void op_bp_from_sp_imm16(State *s, uint16_t imm) {
    uint32_t value = (uint32_t)(uint16_t)s->r[0] + imm;
    unsp_update_nzsc(s, value, (uint16_t)s->r[0], imm);
    s->r[5] = (uint16_t)value;
}

static inline void op_ld_imm16(State *s, int rd, uint16_t imm) {
    s->r[rd] = imm;
    unsp_update_nz(s, imm);
}

static inline void op_ld_ind(State *s, int rd, int rs) {
    uint32_t ea = (uint32_t)(uint16_t)s->r[rs] | ((uint32_t)unsp_ds(s) << 16);
    uint16_t value = unsp_read16(s, ea);
    s->r[rd] = value;
    unsp_update_nz(s, value);
}

static inline void op_ld_ind_postinc(State *s, int rd, int rs) {
    uint32_t ea = (uint32_t)(uint16_t)s->r[rs] | ((uint32_t)unsp_ds(s) << 16);
    uint16_t value = unsp_read16(s, ea);
    s->r[rs] = (uint16_t)(s->r[rs] + 1);
    s->r[rd] = value;
    unsp_update_nz(s, value);
}

static inline void op_st_ind(State *s, int rs, int src) {
    uint32_t ea = (uint32_t)(uint16_t)s->r[rs] | ((uint32_t)unsp_ds(s) << 16);
    unsp_write16(s, ea, (uint16_t)s->r[src]);
}

static inline void op_st_ind_postinc(State *s, int rs, int src) {
    uint32_t ea = (uint32_t)(uint16_t)s->r[rs] | ((uint32_t)unsp_ds(s) << 16);
    unsp_write16(s, ea, (uint16_t)s->r[src]);
    s->r[rs] = (uint16_t)(s->r[rs] + 1);
}

static inline void op_ld_bp_imm(State *s, int rd, uint16_t off) {
    uint16_t value = unsp_read16(s, (uint16_t)(s->r[5] + off));
    s->r[rd] = value;
    unsp_update_nz(s, value);
}

static inline void op_st_bp_imm(State *s, int src, uint16_t off) {
    unsp_write16(s, (uint16_t)(s->r[5] + off), (uint16_t)s->r[src]);
}

static inline void op_xor_ind(State *s, int rd, int rs) {
    uint32_t ea = (uint32_t)(uint16_t)s->r[rs] | ((uint32_t)unsp_ds(s) << 16);
    uint16_t value = unsp_read16(s, ea);
    s->r[rd] ^= value;
    unsp_update_nz(s, s->r[rd]);
}

static inline void op_xor_ind_postinc(State *s, int rd, int rs) {
    uint16_t lhs = (uint16_t)s->r[rd];
    uint32_t ea = (uint32_t)(uint16_t)s->r[rs] | ((uint32_t)unsp_ds(s) << 16);
    uint16_t value = unsp_read16(s, ea);
    s->r[rs] = (uint16_t)(s->r[rs] + 1);
    s->r[rd] = (uint16_t)(lhs ^ value);
    unsp_update_nz(s, s->r[rd]);
}

static inline void op_xor_bp_imm(State *s, int rd, uint16_t off) {
    uint16_t value = unsp_read16(s, (uint16_t)(s->r[5] + off));
    s->r[rd] ^= value;
    unsp_update_nz(s, s->r[rd]);
}

static inline void op_xor_reg(State *s, int rd, int rs) {
    s->r[rd] ^= s->r[rs];
    unsp_update_nz(s, s->r[rd]);
}

static inline void op_lsl_reg(State *s, int rd, int rs) {
    s->r[rd] = (uint16_t)((uint16_t)s->r[rd] << (s->r[rs] & 0x1f));
}

static inline void op_lslor_reg(State *s, int rd, int rs) {
    uint32_t value = (uint32_t)(uint16_t)s->r[rd] << (s->r[rs] & 0x1f);
    s->r[3] = (uint16_t)value;
    s->r[4] = (uint16_t)(s->r[4] | (value >> 16));
}

static inline void op_shift_right_imm(State *s, int rd, int rs, int opn) {
    uint32_t shift = ((uint32_t)(uint16_t)s->r[rs] << 4) | s->sb;
    shift >>= (opn - 3);
    s->sb = (uint16_t)(shift & 0x0f);
    s->r[rd] = (uint16_t)(shift >> 4);
    unsp_update_nz(s, s->r[rd]);
}

static inline void op_rotate_right_imm(State *s, int rd, int rs, int opn) {
    uint32_t shift = (((uint32_t)s->sb << 16) | (uint16_t)s->r[rs]) << 4 | s->sb;
    if (opn & 4) {
        shift >>= (opn - 3);
        s->sb = (uint16_t)(shift & 0x0f);
    } else {
        shift <<= (opn + 1);
        s->sb = (uint16_t)((shift >> 20) & 0x0f);
    }
    s->r[rd] = (uint16_t)(shift >> 4);
    unsp_update_nz(s, s->r[rd]);
}

static inline void op_test_ind_postinc(State *s, int rd, int rs) {
    uint32_t ea = (uint32_t)(uint16_t)s->r[rs] | ((uint32_t)unsp_ds(s) << 16);
    uint16_t value = unsp_read16(s, ea);
    s->r[rs] = (uint16_t)(s->r[rs] + 1);
    unsp_update_nz(s, (uint16_t)((uint16_t)s->r[rd] & value));
}

static inline void op_cmp_imm16(State *s, int rd, uint16_t imm) {
    uint16_t lhs = (uint16_t)s->r[rd];
    uint16_t rhs = (uint16_t)~imm;
    uint32_t value = (uint32_t)lhs + rhs + 1u;
    unsp_update_nzsc(s, value, lhs, rhs);
}

static inline int op_ja(const State *s) {
    return (s->r[6] & (UNSP_Z | UNSP_C)) == UNSP_C;
}

static inline void op_nop(State *s) { (void)s; }

static inline void op_jmp(State *s, uint32_t pc) { s->r[7] = pc; }

static inline void op_ret(State *s) {
    s->r[7] = 0x200343u;
}

#endif