// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <atomic>

namespace android {
namespace base {
namespace childprocess {

struct Affinity {
    std::atomic<DWORD_PTR> baseline{0};
    std::atomic<DWORD_PTR> applied{0};
};

inline Affinity& affinity() {
    static Affinity value;
    return value;
}

// Publish once, after the caller narrows its own process affinity.
inline void setAffinity(DWORD_PTR baseline, DWORD_PTR applied) {
    affinity().baseline.store(baseline);
    affinity().applied.store(applied);
}

inline DWORD_PTR childMask() {
    const auto applied = affinity().applied.load();
    if (!applied) return 0;
    DWORD_PTR current = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &current, &system)) return 0;
    auto mask = affinity().baseline.load() & system;
    // A later external change must not be undone when creating a child.
    if (current != applied) mask &= current;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
                                  &job, sizeof(job), nullptr) &&
        (job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_AFFINITY)) {
        mask &= job.BasicLimitInformation.Affinity;
    }
    return mask;
}

inline bool restoreAffinity(HANDLE process, DWORD_PTR mask) {
    if (!mask) return true;
    DWORD_PTR current = 0, system = 0;
    if (!GetProcessAffinityMask(process, &current, &system)) return false;
    if (!SetProcessAffinityMask(process, mask & system)) return false;
    return GetProcessAffinityMask(process, &current, &system) && current == mask;
}

inline BOOL create(LPCWSTR application, LPWSTR command,
                   LPSECURITY_ATTRIBUTES processAttributes,
                   LPSECURITY_ATTRIBUTES threadAttributes, BOOL inherit,
                   DWORD flags, LPVOID environment, LPCWSTR directory,
                   LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process) {
    const auto mask = childMask();
    if (!CreateProcessW(application, command, processAttributes, threadAttributes,
                        inherit, flags | (mask ? CREATE_SUSPENDED : 0),
                        environment, directory, startup, process)) return FALSE;
    if (mask && (!restoreAffinity(process->hProcess, mask) ||
                 (!(flags & CREATE_SUSPENDED) && ResumeThread(process->hThread) == DWORD(-1)))) {
        const DWORD error = GetLastError();
        TerminateProcess(process->hProcess, 1);
        CloseHandle(process->hThread);
        CloseHandle(process->hProcess);
        *process = {};
        SetLastError(error ? error : ERROR_INVALID_DATA);
        return FALSE;
    }
    return TRUE;
}

} // namespace childprocess
} // namespace base
} // namespace android
