/*
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"

#include "qemu-common.h"
#include "cpu.h"

void x86_cpu_xsave_all_areas(X86CPU *cpu, X86XSaveArea *buf)
{
    CPUX86State *env = &cpu->env;
    X86XSaveArea *xsave = buf;

    uint16_t cwd, swd, twd;
    int i;
    memset(xsave, 0, sizeof(X86XSaveArea));
    twd = 0;
    swd = env->fpus & ~(7 << 11);
    swd |= (env->fpstt & 7) << 11;
    cwd = env->fpuc;
    for (i = 0; i < 8; ++i) {
        twd |= (!env->fptags[i]) << i;
    }
    xsave->legacy.fcw = cwd;
    xsave->legacy.fsw = swd;
    xsave->legacy.ftw = twd;
    xsave->legacy.fpop = env->fpop;
    xsave->legacy.fpip = env->fpip;
    xsave->legacy.fpdp = env->fpdp;
    memcpy(&xsave->legacy.fpregs, env->fpregs,
            sizeof env->fpregs);
    xsave->legacy.mxcsr = env->mxcsr;
    xsave->header.xstate_bv = env->xstate_bv;
    memcpy(&xsave->bndreg_state.bnd_regs, env->bnd_regs,
            sizeof env->bnd_regs);
    xsave->bndcsr_state.bndcsr = env->bndcs_regs;
    memcpy(&xsave->opmask_state.opmask_regs, env->opmask_regs,
            sizeof env->opmask_regs);

    for (i = 0; i < CPU_NB_REGS; i++) {
        uint8_t *xmm = xsave->legacy.xmm_regs[i];
        uint8_t *ymmh = xsave->avx_state.ymmh[i];
        uint8_t *zmmh = xsave->zmm_hi256_state.zmm_hi256[i];
        stq_p(xmm,     env->xmm_regs[i].ZMM_Q(0));
        stq_p(xmm+8,   env->xmm_regs[i].ZMM_Q(1));
        stq_p(ymmh,    env->xmm_regs[i].ZMM_Q(2));
        stq_p(ymmh+8,  env->xmm_regs[i].ZMM_Q(3));
        stq_p(zmmh,    env->xmm_regs[i].ZMM_Q(4));
        stq_p(zmmh+8,  env->xmm_regs[i].ZMM_Q(5));
        stq_p(zmmh+16, env->xmm_regs[i].ZMM_Q(6));
        stq_p(zmmh+24, env->xmm_regs[i].ZMM_Q(7));
    }

#ifdef TARGET_X86_64
    memcpy(&xsave->hi16_zmm_state.hi16_zmm, &env->xmm_regs[16],
            16 * sizeof env->xmm_regs[16]);
    memcpy(&xsave->pkru_state, &env->pkru, sizeof env->pkru);
#endif

}

void x86_cpu_xrstor_all_areas(X86CPU *cpu, const X86XSaveArea *buf)
{

    CPUX86State *env = &cpu->env;
    const X86XSaveArea *xsave = buf;

    int i;
    uint16_t cwd, swd, twd;
    cwd = xsave->legacy.fcw;
    swd = xsave->legacy.fsw;
    twd = xsave->legacy.ftw;
    env->fpop = xsave->legacy.fpop;
    env->fpstt = (swd >> 11) & 7;
    env->fpus = swd;
    env->fpuc = cwd;
    for (i = 0; i < 8; ++i) {
        env->fptags[i] = !((twd >> i) & 1);
    }
    env->fpip = xsave->legacy.fpip;
    env->fpdp = xsave->legacy.fpdp;
    env->mxcsr = xsave->legacy.mxcsr;
    memcpy(env->fpregs, &xsave->legacy.fpregs,
            sizeof env->fpregs);
    env->xstate_bv = xsave->header.xstate_bv;
    memcpy(env->bnd_regs, &xsave->bndreg_state.bnd_regs,
            sizeof env->bnd_regs);
    env->bndcs_regs = xsave->bndcsr_state.bndcsr;
    memcpy(env->opmask_regs, &xsave->opmask_state.opmask_regs,
            sizeof env->opmask_regs);

    for (i = 0; i < CPU_NB_REGS; i++) {
        const uint8_t *xmm = xsave->legacy.xmm_regs[i];
        const uint8_t *ymmh = xsave->avx_state.ymmh[i];
        const uint8_t *zmmh = xsave->zmm_hi256_state.zmm_hi256[i];
        env->xmm_regs[i].ZMM_Q(0) = ldq_p(xmm);
        env->xmm_regs[i].ZMM_Q(1) = ldq_p(xmm+8);
        env->xmm_regs[i].ZMM_Q(2) = ldq_p(ymmh);
        env->xmm_regs[i].ZMM_Q(3) = ldq_p(ymmh+8);
        env->xmm_regs[i].ZMM_Q(4) = ldq_p(zmmh);
        env->xmm_regs[i].ZMM_Q(5) = ldq_p(zmmh+8);
        env->xmm_regs[i].ZMM_Q(6) = ldq_p(zmmh+16);
        env->xmm_regs[i].ZMM_Q(7) = ldq_p(zmmh+24);
    }

#ifdef TARGET_X86_64
    memcpy(&env->xmm_regs[16], &xsave->hi16_zmm_state.hi16_zmm,
           16 * sizeof env->xmm_regs[16]);
    memcpy(&env->pkru, &xsave->pkru_state, sizeof env->pkru);
#endif

}

/*
 * KEMU: XSAVE state de/compaction helpers.
 *
 * Ported from upstream QEMU ("target/i386: add de/compaction to
 * xsave_helper", Magnus Kulke).  Hyper-V (WHPX) uses XSAVES, which stores
 * extended state in compacted format where components are packed
 * contiguously, while QEMU's internal XSAVE representation uses the
 * standard format where each component has a fixed offset.  These helpers
 * convert between the two layouts.
 */

#define XSAVE_XSTATE_BV_OFFSET  offsetof(X86XSaveArea, header.xstate_bv)
#define XSAVE_XCOMP_BV_OFFSET   offsetof(X86XSaveArea, header.xcomp_bv)
#define XSAVE_EXT_OFFSET        (sizeof(X86LegacyXSaveArea) + \
                                 sizeof(X86XSaveHeader))

/**
 * decompact_xsave_area - Convert compacted XSAVE format to standard format
 * @buf: Source buffer containing compacted XSAVE data
 * @buflen: Size of source buffer
 * @env: CPU state whose xsave_buf receives the standard format data
 *
 * Returns: 0 on success, negative errno on failure
 */
int decompact_xsave_area(const void *buf, size_t buflen, CPUX86State *env)
{
    uint64_t compacted_xstate_bv, compacted_xcomp_bv, compacted_layout_bv;
    size_t xsave_offset;
    uint64_t *xcomp_bv;
    size_t i;
    uint32_t eax, ebx, ecx, edx;
    uint32_t size, dst_off, dst_size;
    bool align64, supervisor;
    bool dbg = getenv("KEMU_WHPX_XSAVE_DEBUG") != NULL;
    uint64_t guest_xcr0, *xstate_bv;

    compacted_xstate_bv = *(uint64_t *)(buf + XSAVE_XSTATE_BV_OFFSET);
    compacted_xcomp_bv  = *(uint64_t *)(buf + XSAVE_XCOMP_BV_OFFSET);

    /* This function only handles compacted format (bit 63 set) */
    assert((compacted_xcomp_bv >> 63) & 1);

    if (dbg) {
        fprintf(stderr, "XSAVE decompact: xstate_bv=0x%016" PRIx64
                " xcomp_bv=0x%016" PRIx64 " buflen=%zu\n",
                compacted_xstate_bv, compacted_xcomp_bv, buflen);
    }

    /* Low bits of XCOMP_BV describe which components are in the layout */
    compacted_layout_bv = compacted_xcomp_bv & ~(1ULL << 63);

    /* Zero out buffer, then copy legacy region (FP + SSE) and header as-is */
    memset(env->xsave_buf, 0, env->xsave_buf_len);
    memcpy(env->xsave_buf, buf, XSAVE_EXT_OFFSET);

    /*
     * Mask XSTATE_BV with the guest's supported XCR0 because supervisor
     * state (IA32_XSS) is hypervisor-managed and features disabled at
     * partition creation must be excluded.
     */
    guest_xcr0 = ((uint64_t)env->features[FEAT_XSAVE_COMP_HI] << 32) |
                 env->features[FEAT_XSAVE_COMP_LO];
    xstate_bv = (uint64_t *)(env->xsave_buf + XSAVE_XSTATE_BV_OFFSET);
    *xstate_bv &= guest_xcr0;

    /* Clear bit 63 - output is standard format, not compacted */
    xcomp_bv = (uint64_t *)(env->xsave_buf + XSAVE_XCOMP_BV_OFFSET);
    *xcomp_bv = *xcomp_bv & ~(1ULL << 63);

    /*
     * Process each extended state component in the compacted layout.
     * Components 0 and 1 (FP and SSE) are in the legacy region, so start
     * at component 2.  For each component:
     * - Walk the source with the host geometry (the hypervisor's image)
     * - Map it to the guest model's standard-format slot when known
     * - Copy if the component has non-init state (bit set in XSTATE_BV)
     */
    xsave_offset = XSAVE_EXT_OFFSET;
    for (i = 2; i < 63; i++) {
        if (((compacted_layout_bv >> i) & 1) == 0) {
            continue;
        }

        /*
         * The compacted image is produced by the hypervisor with its own
         * (host) XSAVE capability, so the source layout must be walked with
         * the host component geometry.
         */
        host_cpuid(0xD, i, &eax, &ebx, &ecx, &edx);
        size = eax;
        align64 = (ecx & ESA_FEATURE_ALIGN64_MASK) != 0;
        supervisor = (ecx & ESA_FEATURE_XSS_MASK) != 0;

        if (dbg) {
            fprintf(stderr, "  comp %zu: host size=%u off=%u ecx=0x%x\n",
                    i, size, ebx, ecx);
        }

        if (size == 0) {
            error_report("xsave component %zu: size unknown to host CPUID", i);
            return -EINVAL;
        }

        if (align64) {
            xsave_offset = QEMU_ALIGN_UP(xsave_offset, 64);
        }

        if (xsave_offset + size > buflen) {
            error_report("xsave component %zu overruns source buffer: "
                         "offset=%zu size=%u buflen=%zu",
                         i, xsave_offset, size, buflen);
            return -E2BIG;
        }

        /*
         * KEMU: Copy the component into the guest's standard-format buffer
         * when the guest CPU model knows it and it is not supervisor state.
         * Supervisor components have no slot in the standard buffer (their
         * CPUID offset is 0), and components unknown to the guest model
         * (e.g. AVX-512 with the android64 model) have no destination at
         * all; both are skipped while the source offset keeps advancing.
         */
        if (!supervisor &&
            x86_cpu_get_xsave_component(env, i, &dst_size, &dst_off)) {
            if ((dst_off + dst_size) > env->xsave_buf_len) {
                error_report("xsave component %zu overruns destination "
                             "buffer: offset=%u size=%u buflen=%zu",
                             i, dst_off, dst_size,
                             (size_t)env->xsave_buf_len);
                return -E2BIG;
            }

            if (((compacted_xstate_bv >> i) & 1) != 0) {
                memcpy(env->xsave_buf + dst_off, buf + xsave_offset,
                       MIN(size, dst_size));
            }
        }

        xsave_offset += size;
    }

    return 0;
}

/**
 * compact_xsave_area - Convert standard XSAVE format to compacted format
 * @env: CPU state containing the standard format XSAVE buffer
 * @buf: Destination buffer for compacted XSAVE data (sent to hypervisor)
 * @buflen: Size of destination buffer
 *
 * Returns: total size of compacted XSAVE data written to @buf
 */
int compact_xsave_area(CPUX86State *env, void *buf, size_t buflen)
{
    uint64_t *xcomp_bv;
    size_t i;
    uint32_t eax, ebx, ecx, edx;
    uint32_t size, src_off;
    bool align64;
    size_t compact_offset;
    uint64_t host_xcr0_mask, guest_xcr0;

    /* Zero out buffer, then copy legacy region (FP + SSE) and header as-is */
    memset(buf, 0, buflen);
    memcpy(buf, env->xsave_buf, XSAVE_EXT_OFFSET);

    /*
     * Set XCOMP_BV to indicate compacted format (bit 63) and which
     * components are in the layout.  XCOMP_BV must reflect the
     * partition's XSAVE capability, not the guest's current XCR0, because
     * the hypervisor uses XSAVES based on its own capability.  Intersect
     * the host XCR0 with the guest's supported XCR0 features so that
     * features disabled at partition creation are excluded.
     */
    host_cpuid(0xD, 0, &eax, &ebx, &ecx, &edx);
    host_xcr0_mask = ((uint64_t)edx << 32) | eax;
    guest_xcr0 = ((uint64_t)env->features[FEAT_XSAVE_COMP_HI] << 32) |
                 env->features[FEAT_XSAVE_COMP_LO];
    host_xcr0_mask &= guest_xcr0;
    xcomp_bv = buf + XSAVE_XCOMP_BV_OFFSET;
    *xcomp_bv = host_xcr0_mask | (1ULL << 63);

    /*
     * Process each extended state component in the host's XCR0.  The
     * compacted layout must match XCOMP_BV (host capability).  For each
     * component get its size and standard offset from host CPUID, apply
     * 64-byte alignment if required, copy data only if the guest has the
     * component enabled in XCR0, and always advance the offset to
     * maintain the correct layout.
     */
    compact_offset = XSAVE_EXT_OFFSET;
    for (i = 2; i < 63; i++) {
        if (!((host_xcr0_mask >> i) & 1)) {
            continue;
        }

        /* Query host CPUID for this component's size and standard offset */
        host_cpuid(0xD, i, &eax, &ebx, &ecx, &edx);
        size = eax;
        src_off = ebx;
        align64 = (ecx >> 1) & 1;

        if (size == 0) {
            /* Component in host xcr0 but unknown - shouldn't happen */
            continue;
        }

        /* Apply 64-byte alignment if required by this component */
        if (align64) {
            compact_offset = QEMU_ALIGN_UP(compact_offset, 64);
        }

        /*
         * Only copy data if the guest has this component enabled in XCR0.
         * Otherwise the component remains zeroed (init state), but we
         * still advance the offset to maintain the correct layout.
         */
        if ((env->xcr0 >> i) & 1) {
            memcpy(buf + compact_offset, env->xsave_buf + src_off, size);
        }

        compact_offset += size;
    }

    return compact_offset;
}
