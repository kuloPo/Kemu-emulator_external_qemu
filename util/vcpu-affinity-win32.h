/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <windows.h>
#include <string>
#include "vcpu-affinity-policy.h"

namespace kemu {
namespace affinity {

struct WindowsApi {
    using QuerySets = BOOL (WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    using ReadSets = BOOL (WINAPI*)(HANDLE, PULONG, ULONG, PULONG);
    QuerySets query = nullptr;
    ReadSets defaults = nullptr;
    decltype(&GetLogicalProcessorInformationEx) relationships = GetLogicalProcessorInformationEx;
    decltype(&GetProcessAffinityMask) getMask = GetProcessAffinityMask;
    decltype(&SetProcessAffinityMask) setMask = SetProcessAffinityMask;
    decltype(&GetActiveProcessorGroupCount) groups = GetActiveProcessorGroupCount;
};

WindowsApi systemApi();

class WindowsAffinity {
public:
    void initialize(unsigned count, const WindowsApi& api = systemApi());
    void apply(HANDLE thread, unsigned vcpu) const;
    const Plan& plan() const { return mPlan; }

private:
    bool mInitialized = false;
    unsigned mCount = 0;
    Plan mPlan;
    std::string mPool;
};

} // namespace affinity
} // namespace kemu
