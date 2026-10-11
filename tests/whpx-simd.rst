WHPX SIMD MMIO functional tests
==============================

The WHPX fallback handles memory moves needed by the current x86_64 Android
libc, compiler-inlined ANGLE copies/fills, and ARM64 libc translated by
NativeBridge. It supports legacy and VEX MOVDQU, MOVDQA, MOVUPS/MOVUPD,
MOVAPS/MOVAPD, MOVNTDQ, MOVD/MOVQ, plus legacy MOVSS. Only 64-bit execution
mode is decoded; unsupported instructions continue to WinHvEmulation.

Portable decoder/state checks
-----------------------------

Run from the QEMU source directory::

  cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
      tests/test-whpx-simd.c -o /tmp/test-whpx-simd
  /tmp/test-whpx-simd

The same source is registered as test-whpx-simd in the Linux QEMU test build.

Real WHPX MMIO checks
--------------------

Start a Linux/Android x86_64 guest with this extra QEMU device::

  -device pci-testdev,membar-scratch=on

This opt-in test device provides an 8 KiB MMIO buffer. It deliberately has no
RAM mapping, so passing these tests requires instruction emulation on the
host. The normal pci-testdev behavior is unchanged without the property.
Snapshots are not supported for the scratch buffer; use a cold boot.

Compile tests/whpx-simd-guest.c for the guest with -O2 -fno-builtin and run
it as root, passing the device's sysfs resource0 path. Find the PCI device
with vendor 0x1b36 and device 0x0005 rather than assuming a fixed slot.
For example::

  /data/local/tmp/whpx-simd-guest /sys/bus/pci/devices/0000:00:1b.0/resource0

The tests check exact store widths and guards, unaligned transfers,
noncontiguous physical pages, page/alignment faults, upper-vector state,
and real libc memcpy/memset calls. Set KEMU_WHPX_SIMD_LOG=1 on the host to
log each handled instruction class/direction once per vCPU. Redirected
host output may be buffered until emulator shutdown.

Repeat with KEMU_WHPX_DISABLE_XSAVE=1 to exercise the direct XMM-register
path and the SSE libc dispatch. AVX-specific checks are skipped in this mode.

ANGLE integration checks
------------------------

Build tests/whpx-angle-guest.c with -O2 -fno-builtin, EGL, and GLESv2 for
both x86_64 and ARM64 Android. The ARM64 binary should run through the
guest's NativeBridge executable support. Run each with the same resource0
argument after deploying the ANGLE libraries built without device_copy.h.

The integration test checks real libc copies and zero/nonzero fills on
MMIO at 23 sizes, then creates an ES3 pbuffer using ANGLE. It verifies
texture upload, mapped GPU-buffer memcpy/memset, texture subimage upload,
clear and byte-exact pixel readback at seven sizes. It performs no timing
or performance measurement.
