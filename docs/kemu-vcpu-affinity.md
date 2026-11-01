# Windows vCPU scheduling

Kemu applies one startup scheduling pool to the entire engine process after
resolving the Android CPU configuration and before GPU queries, rendering, UI
and audio initialization. Process hard
affinity constrains existing and future threads, including rendering, audio,
I/O and driver worker threads, to that pool. The common vCPU creation path sets
ideal processor preferences after each thread is created. Shared single-threaded
TCG workers do not receive individual vCPU hints.

No third-party topology library or driver is required. Windows CPU Sets provide
performance classes and processor identities. GetLogicalProcessorInformationEx
provides physical cores and unified L3 cache domains. Neither processor numbering
nor the presence of SMT is assumed.

## Automatic policy

* Heterogeneous processors: if the vCPU count fits the number of physical cores
  in the highest performance class, select all available logical processors in
  that class. Set different physical cores as the initial ideal processors.
  Otherwise, select all available classes and let Windows choose between cores
  and SMT siblings, without assigning particular vCPUs to slower cores.
* Homogeneous processors: prefer the L3 domain containing the lowest available
  processor number. Keep all its available logical processors in the pool.
  Ideal processors use one thread per physical core before SMT siblings. If
  more vCPUs are requested than that domain has logical processors, extend the
  pool into subsequent domains, physical cores first, until capacity is reached.
* Oversubscription: use all available processors, reusing ideal processors when
  necessary. The vCPU count is not changed.

For two L3 domains with SMT pairs (0,1), (2,3), etc. and domains 0-15 and 16-31:

| vCPUs | Process-wide logical processors | vCPU ideal processor order |
| --- | --- | --- |
| 8 | 0-15 | 0,2,4,6,8,10,12,14 |
| 16 | 0-15 | even processors, then odd processors |
| 18 | 0-15,16,18 | first domain as above, then 16,18 |

These are topology-derived examples, not fixed CPU IDs in the implementation.
An L3 domain is not labeled as a physical CCD. No V-cache identity is inferred
from domain order or cache capacity.

All engine threads share the process affinity boundary; they are not pinned
one-to-one or given exclusive core reservations. vCPU ideal processors remain
scheduling preferences within that boundary. Windows services,
other processes and system interrupts are outside this policy. Applying host
thread settings does not establish which physical core a hypervisor uses for
guest execution or prove a frame-time improvement.

## Restrictions and fallback

The available set respects process hard affinity, process default CPU Sets and
sets reserved for other processes. An existing thread CPU Set assignment is
preserved, but cannot override the process hard affinity boundary. Parked cores
remain eligible so Windows can manage power and unpark them. The code verifies
process affinity and ideal processors after application. It does not set thread
CPU Sets; threads without explicit assignments follow process default CPU Sets.
Components can retain narrower thread affinity within the shared boundary.

The pre-policy process affinity is retained for independently running child
tools and restart helpers. Supported child launch paths create the child
suspended, restore that baseline, then resume it before it can create more
children. This covers System::runCommand (including ADB) and Kemu's Qt process
and detached handoff paths. Later external process restrictions and inherited
job affinity are respected. The parent process pool is never temporarily widened.
A restarted engine computes its own pool from the saved CPU count and inherited
baseline. Existing unrelated ADB servers are not modified.

Missing APIs, incomplete or inconsistent topology and systems with more than
one active processor group retain system scheduling. The multi-group fallback
avoids silently confining Windows 11 processes to their primary group. A
failed process-affinity verification attempts to restore the previous process
mask. A vCPU preference failure restores that thread's previous preferences and
retains the shared process boundary. The plan is computed once at startup;
CPU hot-add does not trigger live pool rebalancing.

Set `KEMU_VCPU_AFFINITY=system` in Kemu.exe's launch environment to disable the
policy. The default is `auto`. Invalid values safely select system scheduling.
No persistent machine-wide setting is changed. Startup logs contain the policy,
process-wide pool, vCPU index, host thread ID, processor group and ideal
processor, or the reason for fallback.

## Verification

Build `kemu-vcpu-affinity-tests`, `qemu-system-x86_64` and `Kemu` with the normal
Windows build environment. The engine target is built, never launched directly.

Run `ctest -R "kemu-(vcpu-affinity|affinity-)" --output-on-failure` in the build
directory. Each native or fault-injection scenario runs in a separate process
and produces a Google Test XML report. The implementation is compiled once into
the `kemu-affinity` library, shared by the engine and tests.

Run `kemu-vcpu-affinity-tests.exe` for synthetic topology cases, including
nonadjacent SMT siblings, sparse IDs, multiple performance classes, P cores
without SMT, restricted affinity, missing topology and multi-group fallback.
Run it with `--native 8`, `--native 16` or `--native 18` to exercise the Windows
adapter on actual worker threads. `--native restrict` restricts only the test
process; `--native thread-override` checks an existing
thread assignment, `--native pinned` checks a component's narrower thread mask,
and `--native defaults` checks process default CPU Sets. Native tests also check
existing and newly created host threads, including sampled processor numbers.
Use `--native system` to check the opt-out. Controlled fixtures cover missing
topology/APIs, multi-group fallback, Windows operation failures, rollback, child
inheritance and externally restricted process/job baselines.

For emulator integration, launch the rebuilt runtime through `Kemu.exe`, check
the vCPU settings in its startup log and verify guest boot completion. Compare
real workloads with `auto` and `system` before claiming a performance benefit.

The opt-in `KEMU_GUI_TESTS` build also supports `KEMU_AFFINITY_TEST_REQUEST`.
Set it to a test request file containing `<engine-pid> <new-vcpu-count>`. Once
the UI is ready, the test changes the actual CPU settings control, closes the
settings dialog and accepts its restart prompt. A `.result` file records the
action. Match the PID to each new engine to exercise repeated restarts. This
hook is absent from normal builds. `kemu-restart-tests Kemu.exe` checks launcher
waiting, handle inheritance and affinity with a fixture; `--qt-affinity` checks
the Qt child launch path without starting an emulator.

## API references

* https://learn.microsoft.com/en-us/windows/win32/procthread/cpu-sets
* https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setprocessaffinitymask
* https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformationex
* https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadidealprocessorex
