/* Standalone decoder/state tests: cc -std=c11 -Wall -Wextra -Werror
 * tests/test-whpx-simd.c -o test-whpx-simd && ./test-whpx-simd
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "../target/i386/whpx-simd.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static unsigned tests;
static WhpxSimdInsn decode(const uint8_t *bytes, size_t n)
{
    WhpxSimdInsn d;
    size_t i;
    CHECK(whpx_simd_decode(bytes, n, &d) == 1);
    CHECK(d.length == n);
    for (i = 0; i < n; ++i) {
        WhpxSimdInsn incomplete;
        CHECK(whpx_simd_decode(bytes, i, &incomplete) == -1);
    }
    ++tests;
    return d;
}

int main(void)
{
    const uint8_t sse[][4] = {{0xf3, 0x0f, 0x6f, 0x07},
                              {0xf3, 0x0f, 0x7f, 0x07},
                              {0x66, 0x0f, 0x7f, 0x07},
                              {0x66, 0x0f, 0xe7, 0x07},
                              {0xf3, 0x0f, 0x11, 0x07}};
    const uint8_t vex[][4] = {{0xc5, 0xfe, 0x6f, 0x07},
                              {0xc5, 0xfe, 0x7f, 0x07},
                              {0xc5, 0xfd, 0x7f, 0x07},
                              {0xc5, 0xfc, 0x29, 0x07},
                              {0xc5, 0xfd, 0xe7, 0x07}};
    uint8_t aps[] = {0x0f, 0x29, 0x07};
    uint8_t ups[] = {0x0f, 0x11, 0x07};
    uint8_t movq[] = {0x66, 0x42, 0x0f, 0xd6, 0x04, 0x52};
    uint8_t movq_load[] = {0xf3, 0x0f, 0x7e, 0x07};
    uint8_t movd[] = {0x66, 0x0f, 0x7e, 0x07};
    uint8_t movsd_store[] = {0xf2, 0x41, 0x0f, 0x11, 0x44, 0x24, 0x70};
    uint8_t movsd_load[] = {0xf2, 0x44, 0x0f, 0x10, 0x4f, 0x03};
    uint8_t vmovsd[] = {0xc5, 0xfb, 0x10, 0x07};
    uint8_t movsd_regreg[] = {0xf2, 0x0f, 0x10, 0xc1};
    uint8_t sib[] = {0xf3, 0x47, 0x0f, 0x7f, 0x4c, 0xa5, 0xe0};
    uint8_t riprel[] = {0xf3, 0x0f, 0x6f, 0x05, 0xf0, 0xff, 0xff, 0xff};
    uint8_t fs32[] = {0x64, 0x67, 0xf3, 0x0f, 0x7f, 0x07};
    uint8_t vex3[] = {0xc4, 0x01, 0x7e, 0x7f, 0x4c, 0xa5, 0xe0};
    uint8_t absolute[] = {0xf3, 0x0f, 0x7f, 0x04, 0x25, 0x00, 0x20, 0x00, 0x00};
    uint8_t regreg[] = {0xf3, 0x0f, 0x7f, 0xc0};
    uint8_t badvvvv[] = {0xc5, 0xee, 0x7f, 0x07};
    uint8_t lock[] = {0xf0, 0xf3, 0x0f, 0x7f, 0x07};
    uint8_t evex[] = {0x62, 0xf1, 0x7e, 0x48, 0x7f, 0x07};
    uint8_t data[32], vector[64], overlong[16];
    uint64_t regs[16] = {0};
    WhpxSimdInsn d;
    unsigned i;

    for (i = 0; i < sizeof(sse) / sizeof(sse[0]); ++i) {
        d = decode(sse[i], 4);
        CHECK(!d.vex && d.base == 7 && d.reg == 0);
        CHECK(d.store == (i != 0));
        CHECK(d.size == (i == 4 ? 4 : 16));
        CHECK(d.aligned == (i == 2 || i == 3));
    }
    for (i = 0; i < sizeof(vex) / sizeof(vex[0]); ++i) {
        d = decode(vex[i], 4);
        CHECK(d.vex && d.size == 32 && d.store == (i != 0));
        CHECK(d.aligned == (i >= 2));
    }
    d = decode(aps, sizeof(aps));
    CHECK(d.aligned && d.store);
    d = decode(ups, sizeof(ups));
    CHECK(!d.aligned && d.store);
    d = decode(movq, sizeof(movq));
    CHECK(d.size == 8 && d.store && d.index == 10);
    d = decode(movq_load, sizeof(movq_load));
    CHECK(d.size == 8 && !d.store);
    d = decode(movd, sizeof(movd));
    CHECK(d.size == 4 && d.store);
    d = decode(movsd_store, sizeof(movsd_store));
    CHECK(d.size == 8 && d.store && d.scalar && !d.aligned && !d.vex);
    CHECK(d.reg == 0 && d.base == 12 && d.index == -1);
    regs[12] = 0x7fe481d0;
    CHECK(whpx_simd_address(&d, regs, 0, 0, 0) == 0x7fe48240);
    d = decode(movsd_load, sizeof(movsd_load));
    CHECK(d.size == 8 && !d.store && d.scalar && !d.aligned && !d.vex);
    CHECK(d.reg == 9 && d.base == 7 && d.displacement == 3);
    regs[7] = 0x1000;
    CHECK(whpx_simd_address(&d, regs, 0, 0, 0) == 0x1003);
    regs[13] = 0x5000;
    regs[12] = 3;
    d = decode(sib, sizeof(sib));
    CHECK(d.reg == 9 && d.base == 13 && d.index == 12 && d.scale == 2);
    CHECK(whpx_simd_address(&d, regs, 0, 0, 0) == 0x4fec);
    d = decode(vex3, sizeof(vex3));
    CHECK(d.reg == 9 && d.vex && d.base == 13 && d.index == 12);
    CHECK(whpx_simd_address(&d, regs, 0, 0, 0) == 0x4fec);
    d = decode(riprel, sizeof(riprel));
    CHECK(whpx_simd_address(&d, regs, 0x1000, 0, 0) == 0xff8);
    d = decode(absolute, sizeof(absolute));
    CHECK(whpx_simd_address(&d, regs, 0x9999, 0, 0) == 0x2000);
    regs[7] = 0x100000fffULL;
    d = decode(fs32, sizeof(fs32));
    CHECK(whpx_simd_address(&d, regs, 0, 0x700000, 0) == 0x700fff);
    CHECK(whpx_simd_decode(regreg, sizeof(regreg), &d) == 0);
    CHECK(whpx_simd_decode(badvvvv, sizeof(badvvvv), &d) == 0);
    CHECK(whpx_simd_decode(lock, sizeof(lock), &d) == 0);
    CHECK(whpx_simd_decode(evex, sizeof(evex), &d) == 0);
    CHECK(whpx_simd_decode(vmovsd, sizeof(vmovsd), &d) == 0);
    CHECK(whpx_simd_decode(movsd_regreg, sizeof(movsd_regreg), &d) == 0);
    memset(overlong, 0x67, sizeof(overlong));
    CHECK(whpx_simd_decode(overlong, sizeof(overlong), &d) == 0);

    for (i = 0; i < sizeof(data); ++i) {
        data[i] = (uint8_t)(i + 1);
    }
    d = decode(sse[0], 4);
    memset(vector, 0xa5, sizeof(vector));
    whpx_simd_load(&d, vector, data);
    CHECK(!memcmp(vector, data, 16));
    for (i = 16; i < 64; ++i) {
        CHECK(vector[i] == 0xa5);
    }
    d = decode(sse[4], 4);
    d.store = false;
    whpx_simd_load(&d, vector, data);
    CHECK(!memcmp(vector, data, 4));
    for (i = 4; i < 16; ++i) {
        CHECK(vector[i] == 0);
    }
    for (i = 16; i < 64; ++i) {
        CHECK(vector[i] == 0xa5);
    }
    d = decode(movsd_load, sizeof(movsd_load));
    memset(vector, 0xa5, sizeof(vector));
    whpx_simd_load(&d, vector, data);
    CHECK(!memcmp(vector, data, 8));
    for (i = 8; i < 16; ++i) {
        CHECK(vector[i] == 0);
    }
    for (i = 16; i < 64; ++i) {
        CHECK(vector[i] == 0xa5);
    }
    d = decode(vex[0], 4);
    whpx_simd_load(&d, vector, data);
    CHECK(!memcmp(vector, data, 32));
    for (i = 32; i < 64; ++i) {
        CHECK(vector[i] == 0);
    }
    printf("PASS: %u instruction cases, all truncations, addressing, rejection "
           "and vector preservation\n",
           tests);
    return 0;
}
