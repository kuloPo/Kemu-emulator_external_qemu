/*
 * SIMD moves used by guest libc and compiler-inlined memory copies.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This deliberately small decoder accepts memory operands in 64-bit mode
 * only. Other instructions/modes remain with WinHvEmulation. No host SIMD
 * instructions are executed on untrusted guest addresses.
 */
#ifndef WHPX_SIMD_H
#define WHPX_SIMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct WhpxSimdInsn {
    uint8_t length, size, reg, segment, kind;
    int8_t base, index;
    uint8_t scale;
    int32_t displacement;
    bool store, aligned, vex, addr32, rip_relative, scalar;
} WhpxSimdInsn;

/* 1: decoded; 0: outside supported subset; -1: need more instruction bytes. */
static inline int whpx_simd_decode(const uint8_t *code, size_t size,
                                   WhpxSimdInsn *insn)
{
    size_t p = 0;
    unsigned b, pp = 0, rex = 0, op, modrm, mod, rm, ext_r, ext_x, ext_b;
    unsigned l = 0, disp_size = 0, i, w;
    uint32_t disp = 0;
    WhpxSimdInsn d = {0};

#define SIMD_BYTE(v)                                                           \
    do {                                                                       \
        if (p == 15) {                                                         \
            return 0;                                                          \
        }                                                                      \
        if (p >= size) {                                                       \
            return -1;                                                         \
        }                                                                      \
        (v) = code[p++];                                                       \
    } while (0)

    d.base = d.index = -1;
    for (;;) {
        SIMD_BYTE(b);
        if (b == 0x66 || b == 0xf3 || b == 0xf2) {
            unsigned next = b == 0x66 ? 1 : b == 0xf3 ? 2 : 3;
            if (pp && pp != next) {
                return 0;
            }
            pp = next;
            rex = 0;
        } else if (b == 0x67) {
            d.addr32 = true;
            rex = 0;
        } else if (b == 0x64 || b == 0x65 || b == 0x26 || b == 0x2e ||
                   b == 0x36 || b == 0x3e) {
            d.segment = b;
            rex = 0;
        } else if ((b & 0xf0) == 0x40) {
            rex = b;
        } else {
            break;
        }
    }
    ext_r = (rex >> 2) & 1;
    ext_x = (rex >> 1) & 1;
    ext_b = rex & 1;
    w = (rex >> 3) & 1;
    if (b == 0xc4 || b == 0xc5) {
        unsigned v1, v2;
        if (rex || pp) {
            return 0;
        }
        d.vex = true;
        SIMD_BYTE(v1);
        ext_r = (~v1 >> 7) & 1;
        if (b == 0xc4) {
            if ((v1 & 31) != 1) {
                return 0;
            } /* 0F map only */
            ext_x = (~v1 >> 6) & 1;
            ext_b = (~v1 >> 5) & 1;
            SIMD_BYTE(v2);
            w = v2 >> 7;
        } else {
            v2 = v1;
        }
        if ((v2 & 0x78) != 0x78) {
            return 0;
        } /* reserved vvvv */
        pp = v2 & 3;
        l = (v2 >> 2) & 1;
    } else if (b != 0x0f) {
        return 0;
    }
    SIMD_BYTE(op);
    d.size = l ? 32 : 16;
    switch (op) {
    case 0x6f:
    case 0x7f: /* MOVDQA / MOVDQU */
        if (pp != 1 && pp != 2) {
            return 0;
        }
        d.store = op == 0x7f;
        d.aligned = pp == 1;
        d.kind = pp == 1 ? 0 : 1;
        break;
    case 0x28:
    case 0x29: /* MOVAPS / MOVAPD */
        if (pp > 1) {
            return 0;
        }
        d.store = op == 0x29;
        d.aligned = true;
        d.kind = 2;
        break;
    case 0x10:
    case 0x11: /* MOVUPS / MOVUPD / legacy MOVSS / MOVSD */
        if (pp >= 2 && d.vex) {
            return 0;
        }
        d.store = op == 0x11;
        d.scalar = pp >= 2;
        d.kind = d.scalar ? 4 : 3;
        if (d.scalar) {
            d.size = pp == 3 ? 8 : 4;
        }
        break;
    case 0xe7: /* MOVNTDQ */
        if (pp != 1) {
            return 0;
        }
        d.store = d.aligned = true;
        d.kind = 5;
        break;
    case 0x6e:
    case 0x7e: /* MOVD/Q; F3 0F 7E loads MOVQ */
        if (l || (pp != 1 && !(op == 0x7e && pp == 2))) {
            return 0;
        }
        d.size = pp == 2 || w ? 8 : 4;
        d.store = op == 0x7e && pp == 1;
        d.scalar = true;
        d.kind = d.size == 8 ? 6 : 7;
        break;
    case 0xd6: /* MOVQ m64, xmm */
        if (pp != 1 || l) {
            return 0;
        }
        d.size = 8;
        d.store = d.scalar = true;
        d.kind = 6;
        break;
    default:
        return 0;
    }
    SIMD_BYTE(modrm);
    mod = modrm >> 6;
    if (mod == 3) {
        return 0;
    }
    d.reg = ((modrm >> 3) & 7) | (ext_r << 3);
    rm = modrm & 7;
    if (rm == 4) {
        unsigned sib, index, base;
        SIMD_BYTE(sib);
        d.scale = sib >> 6;
        index = ((sib >> 3) & 7) | (ext_x << 3);
        base = sib & 7;
        if (index != 4) {
            d.index = index;
        }
        if (mod == 0 && base == 5) {
            disp_size = 4;
        } else {
            d.base = base | (ext_b << 3);
        }
    } else if (mod == 0 && rm == 5) {
        d.rip_relative = true;
        disp_size = 4;
    } else {
        d.base = rm | (ext_b << 3);
    }
    if (mod == 1) {
        disp_size = 1;
    }
    if (mod == 2) {
        disp_size = 4;
    }
    for (i = 0; i < disp_size; ++i) {
        SIMD_BYTE(b);
        disp |= (uint32_t)b << (i * 8);
    }
    d.displacement = disp_size == 1 ? (int8_t)disp : (int32_t)disp;
    if (d.vex) {
        d.kind += 8;
    }
    d.length = p;
    *insn = d;
    return 1;
#undef SIMD_BYTE
}

static inline uint64_t whpx_simd_address(const WhpxSimdInsn *d,
                                         const uint64_t regs[16], uint64_t rip,
                                         uint64_t fs, uint64_t gs)
{
    uint64_t addr = (uint64_t)(int64_t)d->displacement;
    if (d->rip_relative) {
        addr += rip + d->length;
    }
    if (d->base >= 0) {
        addr += regs[d->base];
    }
    if (d->index >= 0) {
        addr += regs[d->index] << d->scale;
    }
    if (d->addr32) {
        addr = (uint32_t)addr;
    }
    if (d->segment == 0x64) {
        addr += fs;
    }
    if (d->segment == 0x65) {
        addr += gs;
    }
    return addr;
}

/* VEX loads clear the rest of the architectural vector register; legacy
 * scalar memory loads clear XMM above the operand and preserve upper YMM/ZMM. */
static inline void whpx_simd_load(const WhpxSimdInsn *d, uint8_t reg[64],
                                  const uint8_t *data)
{
    if (d->vex) {
        memset(reg, 0, 64);
    } else if (d->scalar) {
        memset(reg, 0, 16);
    }
    memcpy(reg, data, d->size);
}
#endif
