/* Run as root in an x86_64 Linux/Android guest with
 * -device pci-testdev,membar-scratch=on. Argument: its sysfs resource0 path.
 * These assembly probes intentionally force individual MMIO instructions;
 * the final tests call the guest's real libc memcpy/memset.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <cpuid.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);               \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
#define AVX __attribute__((target("avx2"), noinline))
#define SSE_STORE(name, op)                                                    \
    static void name(void *p, const void *src)                                 \
    {                                                                          \
        __asm__ volatile("movdqu (%1), %%xmm9\n\t" op " %%xmm9, (%0)"          \
                         :                                                     \
                         : "r"(p), "r"(src)                                    \
                         : "xmm9", "memory");                                  \
    }
#define AVX_STORE(name, op)                                                    \
    static AVX void name(void *p, const void *src)                             \
    {                                                                          \
        __asm__ volatile("vmovdqu (%1), %%ymm9\n\t" op                         \
                         " %%ymm9, (%0)\n\tsfence"                             \
                         :                                                     \
                         : "r"(p), "r"(src)                                    \
                         : "ymm9", "memory");                                  \
    }
SSE_STORE(store_dqu, "movdqu")
SSE_STORE(store_dqa, "movdqa")
SSE_STORE(store_aps, "movaps")
SSE_STORE(store_ups, "movups")
SSE_STORE(store_ntdq, "movntdq")
SSE_STORE(store_ss, "movss")
SSE_STORE(store_sd, "movsd")
SSE_STORE(store_q, "movq")
SSE_STORE(store_d, "movd")
AVX_STORE(store_vdqu, "vmovdqu")
AVX_STORE(store_vdqa, "vmovdqa")
AVX_STORE(store_vaps, "vmovaps")
AVX_STORE(store_vntdq, "vmovntdq")

static void load_dqu(void *out, const void *p)
{
    __asm__ volatile("movdqu (%1), %%xmm9\n\tmovdqu %%xmm9, (%0)"
                     :
                     : "r"(out), "r"(p)
                     : "xmm9", "memory");
}
static void load_sd(void *out, const void *p)
{
    __asm__ volatile("pcmpeqb %%xmm9, %%xmm9\n\tmovsd (%1), %%xmm9\n\t"
                     "movdqu %%xmm9, (%0)"
                     :
                     : "r"(out), "r"(p)
                     : "xmm9", "memory");
}
static AVX void load_sd_preserve(void *out, const void *p, const void *seed)
{
    __asm__ volatile("vmovdqu (%2), %%ymm9\n\tmovsd (%1), %%xmm9\n\t"
                     "vmovdqu %%ymm9, (%0)"
                     :
                     : "r"(out), "r"(p), "r"(seed)
                     : "ymm9", "memory");
}
static AVX void load_vdqu(void *out, const void *p)
{
    __asm__ volatile("vmovdqu (%1), %%ymm9\n\tvmovdqu %%ymm9, (%0)"
                     :
                     : "r"(out), "r"(p)
                     : "ymm9", "memory");
}
static AVX void load_preserve(void *out, const void *p, const void *seed,
                              int vex)
{
    if (vex) {
        __asm__ volatile("vmovdqu (%2), %%ymm9\n\tvmovdqu (%1), "
                         "%%xmm9\n\tvmovdqu %%ymm9, (%0)"
                         :
                         : "r"(out), "r"(p), "r"(seed)
                         : "ymm9", "memory");
    } else {
        __asm__ volatile("vmovdqu (%2), %%ymm9\n\tmovdqu (%1), "
                         "%%xmm9\n\tvmovdqu %%ymm9, (%0)"
                         :
                         : "r"(out), "r"(p), "r"(seed)
                         : "ymm9", "memory");
    }
}

static sigjmp_buf fault_jmp;
static volatile sig_atomic_t expected_fault;
static void fault_handler(int sig)
{
    if (!expected_fault) {
        _exit(128 + sig);
    }
    expected_fault = 0;
    siglongjmp(fault_jmp, 1);
}
static void expect_fault(void (*store)(void *, const void *), void *p,
                         const void *src)
{
    expected_fault = 1;
    if (!sigsetjmp(fault_jmp, 1)) {
        store(p, src);
        CHECK(!"expected SIGSEGV");
    }
    CHECK(!expected_fault);
}
static int has_avx2(void)
{
    unsigned a, b, c, d, lo, hi;
    __cpuid(1, a, b, c, d);
    if ((c & ((1u << 27) | (1u << 28))) != ((1u << 27) | (1u << 28))) {
        return 0;
    }
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6) {
        return 0;
    }
    __cpuid_count(7, 0, a, b, c, d);
    return !!(b & (1u << 5));
}

int main(int argc, char **argv)
{
    struct Case {
        const char *name;
        unsigned size, aligned, avx;
        void (*store)(void *, const void *);
    };
    const struct Case cases[] = {
        {"MOVDQU", 16, 0, 0, store_dqu},   {"MOVDQA", 16, 1, 0, store_dqa},
        {"MOVAPS", 16, 1, 0, store_aps},   {"MOVUPS", 16, 0, 0, store_ups},
        {"MOVNTDQ", 16, 1, 0, store_ntdq}, {"MOVSS", 4, 0, 0, store_ss},
        {"MOVSD", 8, 0, 0, store_sd},
        {"MOVQ", 8, 0, 0, store_q},        {"MOVD", 4, 0, 0, store_d},
        {"VMOVDQU", 32, 0, 1, store_vdqu}, {"VMOVDQA", 32, 1, 1, store_vdqa},
        {"VMOVAPS", 32, 1, 1, store_vaps}, {"VMOVNTDQ", 32, 1, 1, store_vntdq}};
    const unsigned offsets[] = {64, 65, 71, 79, 95, 4065, 4089};
    const unsigned sizes[] = {0,   1,   2,   3,   4,   7,    8,   15,
                              16,  17,  31,  32,  33,  63,   64,  65,
                              127, 128, 129, 255, 256, 1024, 4097};
    uint8_t input[8192], output[8192];
    void *(*volatile copyfn)(void *, const void *, size_t) = memcpy;
    void *(*volatile setfn)(void *, int, size_t) = memset;
    volatile uint8_t *mem, *reverse;
    uint8_t *guard;
    int fd, avx = has_avx2();
    unsigned i, j, k, total = 0;

    CHECK(argc == 2);
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGSEGV, fault_handler);
    signal(SIGBUS, fault_handler);
    fd = open(argv[1], O_RDWR | O_SYNC);
    CHECK(fd >= 0);
    mem = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(mem != MAP_FAILED);
    for (i = 0; i < sizeof(input); ++i) {
        input[i] = (uint8_t)(i * 37 + 11);
    }
    printf("MMIO functional test, AVX2=%d\n", avx);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const struct Case *c = &cases[i];
        if (c->avx && !avx) {
            continue;
        }
        for (j = 0; j < sizeof(offsets) / sizeof(offsets[0]); ++j) {
            unsigned off = offsets[j];
            if (c->aligned && (off % c->size)) {
                continue;
            }
            for (k = off - 8; k < off + c->size + 8; ++k) {
                mem[k] = 0xa5;
            }
            c->store((void *)(mem + off), input);
            __asm__ volatile("sfence" : : : "memory");
            for (k = 0; k < c->size; ++k) {
                CHECK(mem[off + k] == input[k]);
            }
            for (k = 1; k <= 8; ++k) {
                CHECK(mem[off - k] == 0xa5);
                CHECK(mem[off + c->size + k - 1] == 0xa5);
            }
            ++total;
        }
        printf("PASS %s stores and guards\n", c->name);
    }
    for (j = 0; j < sizeof(offsets) / sizeof(offsets[0]); ++j) {
        unsigned off = offsets[j];
        for (k = 0; k < 32; ++k) {
            mem[off + k] = input[k];
        }
        load_dqu(output, (void *)(mem + off));
        CHECK(!memcmp(output, input, 16));
        ++total;
        load_sd(output, (void *)(mem + off));
        CHECK(!memcmp(output, input, 8));
        for (k = 8; k < 16; ++k) {
            CHECK(output[k] == 0);
        }
        ++total;
        if (avx) {
            load_sd_preserve(output, (void *)(mem + off), input + 64);
            CHECK(!memcmp(output, input, 8));
            for (k = 8; k < 16; ++k) {
                CHECK(output[k] == 0);
            }
            CHECK(!memcmp(output + 16, input + 80, 16));
            ++total;
            load_vdqu(output, (void *)(mem + off));
            CHECK(!memcmp(output, input, 32));
            ++total;
            load_preserve(output, (void *)(mem + off), input + 64, 0);
            CHECK(!memcmp(output, input, 16));
            CHECK(!memcmp(output + 16, input + 80, 16));
            ++total;
            load_preserve(output, (void *)(mem + off), input + 64, 1);
            CHECK(!memcmp(output, input, 16));
            for (k = 16; k < 32; ++k) {
                CHECK(output[k] == 0);
            }
            ++total;
        }
    }
    printf(avx ? "PASS SIMD loads, legacy upper-YMM preservation and VEX zeroing\n"
               : "PASS SIMD loads (AVX state tests skipped: unavailable)\n");
    /* Reverse the physical page order: crossing a GVA page boundary must
     * translate again, not continue at first_gpa + length. */
    reverse = mmap(NULL, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(reverse != MAP_FAILED);
    CHECK(mmap((void *)reverse, 4096, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_FIXED, fd, 4096) == (void *)reverse);
    CHECK(mmap((void *)(reverse + 4096), 4096, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_FIXED, fd, 0) == (void *)(reverse + 4096));
    store_dqu((void *)(reverse + 4089), input);
    load_dqu(output, (void *)(reverse + 4089));
    CHECK(!memcmp(output, input, 16));
    ++total;
    store_sd((void *)(reverse + 4089), input);
    load_sd(output, (void *)(reverse + 4089));
    CHECK(!memcmp(output, input, 8));
    ++total;
    if (avx) {
        store_vdqu((void *)(reverse + 4089), input);
        load_vdqu(output, (void *)(reverse + 4089));
        CHECK(!memcmp(output, input, 32));
        ++total;
    }
    printf("PASS cross-page access with noncontiguous physical pages\n");
    guard = mmap(NULL, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guard != MAP_FAILED);
    CHECK(mmap(guard, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
               0) == guard);
    for (k = 4080; k < 4096; ++k) {
        ((volatile uint8_t *)guard)[k] = 0xa5;
    }
    expect_fault(store_dqu, guard + 4089, input);
    expect_fault(store_sd, guard + 4093, input);
    for (k = 4080; k < 4096; ++k) {
        CHECK(((volatile uint8_t *)guard)[k] == 0xa5);
    }
    expect_fault(store_dqa, (void *)(mem + 65), input);
    if (avx) {
        expect_fault(store_vdqu, guard + 4080, input);
        expect_fault(store_vdqa, (void *)(mem + 65), input);
    }
    printf("PASS page/alignment faults without partial stores\n");
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        unsigned n = sizes[i];
        for (k = 0; k < n + 2; ++k) {
            mem[k] = 0xa5;
        }
        CHECK(copyfn((void *)(mem + 1), input, n) == (void *)(mem + 1));
        for (k = 0; k < n; ++k) {
            CHECK(mem[k + 1] == input[k]);
        }
        CHECK(mem[0] == 0xa5 && mem[n + 1] == 0xa5);
        copyfn(output, (void *)(mem + 1), n);
        CHECK(!memcmp(output, input, n));
        CHECK(setfn((void *)(mem + 1), 0x6b, n) == (void *)(mem + 1));
        for (k = 0; k < n; ++k) {
            CHECK(mem[k + 1] == 0x6b);
        }
        CHECK(mem[0] == 0xa5 && mem[n + 1] == 0xa5);
        total += 3;
    }
    printf("PASS real libc memcpy/memset, %u total functional cases\n", total);
    munmap((void *)mem, 8192);
    munmap((void *)reverse, 8192);
    munmap(guard, 8192);
    close(fd);
    return 0;
}
