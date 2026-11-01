/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include "qemu/vcpu-affinity.h"
#include "aemu/base/logging/CLog.h"
#include "vcpu-affinity-win32.h"
#include "../android/android-emu-base/android/base/system/Win32ChildProcess.h"

namespace {
using namespace kemu::affinity;
WindowsAffinity runtime;
std::once_flag initialized;

/* A private error type isolates libraries with different MSVC exception ABIs. */
struct AffinityError {
    const char* reason;
};

void require(bool success, const char* reason)
{
    if (!success) {
        throw AffinityError{reason};
    }
}

template<class Query>
std::vector<ULONG> readSets(Query query, HANDLE object)
{
    ULONG count = 0;
    const BOOL ok = query(object, nullptr, 0, &count);
    require(ok || GetLastError() == ERROR_INSUFFICIENT_BUFFER,
            "cannot query existing CPU Sets");
    std::vector<ULONG> ids(count);
    if (count) {
        require(query(object, ids.data(), count, &count) != FALSE,
                "cannot read existing CPU Sets");
        require(count <= ids.size(), "CPU Sets changed during query");
        ids.resize(count);
    }
    return ids;
}

template<class Query>
std::vector<unsigned char> readTopology(Query query)
{
    DWORD size = 0;
    require(!query(nullptr, &size) && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
            "cannot query CPU topology");
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        require(size && size <= 16 * 1024 * 1024, "invalid CPU topology size");
        std::vector<unsigned char> bytes(size);
        if (query(bytes.data(), &size)) {
            require(size <= bytes.size(), "invalid CPU topology length");
            bytes.resize(size);
            return bytes;
        }
        require(GetLastError() == ERROR_INSUFFICIENT_BUFFER,
                "cannot read CPU topology");
    }
    throw AffinityError{"CPU topology changed during query"};
}

std::vector<Cpu> discover(const WindowsApi& api)
{
    /* CPU Sets and hard affinity interact differently across Windows versions.
     * Do not accidentally restrict a multi-group process to its primary group. */
    require(api.groups() == 1,
            "multi-group topology uses system scheduling");
    DWORD_PTR processMask = 0, systemMask = 0;
    require(api.getMask(GetCurrentProcess(), &processMask, &systemMask) &&
            processMask, "cannot read process affinity");
    const auto defaultIds = readSets(api.defaults, GetCurrentProcess());
    const auto bytes = readTopology([&](void* buffer, DWORD* size) {
        ULONG used = *size;
        const BOOL ok = api.query(static_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer),
                              used, &used, GetCurrentProcess(), 0);
        *size = used;
        return ok;
    });
    std::vector<Cpu> cpus;
    for (size_t offset = 0; offset < bytes.size();) {
        require(bytes.size() - offset >= 2 * sizeof(DWORD), "invalid CPU Set header");
        const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(
                bytes.data() + offset);
        require(entry->Size >= 2 * sizeof(DWORD) &&
                entry->Size <= bytes.size() - offset, "invalid CPU Set record");
        if (entry->Type == CpuSetInformation) {
            require(entry->Size >= sizeof(SYSTEM_CPU_SET_INFORMATION),
                    "incomplete CPU Set record");
            const auto& c = entry->CpuSet;
            require(c.LogicalProcessorIndex < sizeof(DWORD_PTR) * 8,
                    "invalid logical processor number");
            if ((processMask & (DWORD_PTR(1) << c.LogicalProcessorIndex)) &&
                (!c.Allocated || c.AllocatedToTargetProcess) &&
                (defaultIds.empty() || std::find(defaultIds.begin(), defaultIds.end(),
                                                c.Id) != defaultIds.end())) {
                cpus.push_back({c.Id, c.Group, c.LogicalProcessorIndex,
                                c.EfficiencyClass, unknownTopology, unknownTopology});
            }
        }
        offset += entry->Size;
    }

    const auto relationships = readTopology([&](void* buffer, DWORD* size) {
        return api.relationships(RelationAll,
                static_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer), size);
    });
    uint32_t core = 0, cache = 0;
    for (size_t offset = 0; offset < relationships.size();) {
        require(relationships.size() - offset >= 2 * sizeof(DWORD),
                "invalid processor relationship header");
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                relationships.data() + offset);
        require(entry->Size >= 2 * sizeof(DWORD) &&
                entry->Size <= relationships.size() - offset,
                "invalid processor relationship record");
        const GROUP_AFFINITY* masks = nullptr;
        unsigned count = 0;
        size_t maskOffset = 0;
        if (entry->Relationship == RelationProcessorCore) {
            maskOffset = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) +
                         offsetof(PROCESSOR_RELATIONSHIP, GroupMask);
            require(entry->Size >= maskOffset, "incomplete core relationship");
            masks = entry->Processor.GroupMask;
            count = entry->Processor.GroupCount;
            ++core;
        } else if (entry->Relationship == RelationCache) {
            maskOffset = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache) +
                         offsetof(CACHE_RELATIONSHIP, GroupMask);
            require(entry->Size >= maskOffset, "incomplete cache relationship");
            if (entry->Cache.Level == 3 && entry->Cache.Type == CacheUnified) {
                masks = &entry->Cache.GroupMask;
                /* Windows 10 reports zero in the reserved GroupCount field. */
                count = std::max(1u, unsigned(entry->Cache.GroupCount));
                ++cache;
            }
        }
        if (masks) {
            require(count && count <= (entry->Size - maskOffset) / sizeof(*masks),
                    "invalid processor relationship masks");
            for (auto& cpu : cpus) {
                for (unsigned i = 0; i < count; ++i) {
                    if (cpu.group == masks[i].Group &&
                        (masks[i].Mask & (KAFFINITY(1) << cpu.logical))) {
                        auto& id = entry->Relationship == RelationProcessorCore ?
                                   cpu.core : cpu.cache;
                        require(id == unknownTopology, "ambiguous processor topology");
                        id = entry->Relationship == RelationProcessorCore ? core : cache;
                    }
                }
            }
        }
        offset += entry->Size;
    }
    return cpus;
}

std::string applyProcessPool(const Plan& plan, const WindowsApi& api)
{
    DWORD_PTR mask = 0;
    std::string pool;
    for (const auto& cpu : plan.pool) {
        mask |= DWORD_PTR(1) << cpu.logical;
        if (!pool.empty()) {
            pool += ',';
        }
        pool += std::to_string(cpu.logical);
    }
    DWORD_PTR previous = 0, system = 0;
    require(api.getMask(GetCurrentProcess(), &previous, &system) &&
            mask && (mask & previous) == mask,
            "process affinity changed during initialization");
    /* A process-wide boundary also covers existing and future host threads,
     * including threads that select their own CPU Sets. */
    require(api.setMask(GetCurrentProcess(), mask) != FALSE,
            "cannot set process affinity");
    DWORD_PTR actual = 0;
    if (!api.getMask(GetCurrentProcess(), &actual, &system) || actual != mask) {
        if (!api.setMask(GetCurrentProcess(), previous)) {
            dinfo("Kemu affinity: could not restore process affinity");
        }
        throw AffinityError{"process affinity verification failed"};
    }
    dinfo("Kemu affinity: scope=process policy=%s group=%u pool=%s",
          policyName(plan.policy), unsigned(plan.pool.front().group), pool.c_str());
    android::base::childprocess::setAffinity(previous, mask);
    return pool;
}

} // namespace

namespace kemu {
namespace affinity {

WindowsApi systemApi()
{
    WindowsApi api;
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    api.query = reinterpret_cast<WindowsApi::QuerySets>(GetProcAddress(kernel, "GetSystemCpuSetInformation"));
    api.defaults = reinterpret_cast<WindowsApi::ReadSets>(GetProcAddress(kernel, "GetProcessDefaultCpuSets"));
    return api;
}

void WindowsAffinity::initialize(unsigned count, const WindowsApi& api)
{
    if (mInitialized) return;
    mInitialized = true;
    mCount = count;
    const char* mode = std::getenv("KEMU_VCPU_AFFINITY");
    if (mode && std::string(mode) == "system") {
        dinfo("Kemu vCPU affinity: policy=system (requested)");
        return;
    }
    try {
        require(!mode || !*mode || std::string(mode) == "auto",
                "invalid KEMU_VCPU_AFFINITY (expected auto or system)");
        require(api.query && api.defaults, "CPU Set APIs unavailable");
        mPlan = makePlan(discover(api), count);
        require(mPlan.policy != Policy::System, "incomplete or unsupported CPU topology");
        mPool = applyProcessPool(mPlan, api);
    } catch (const AffinityError& error) {
        mPlan = Plan{};
        dinfo("Kemu vCPU affinity: policy=system (%s)", error.reason);
    } catch (...) {
        mPlan = Plan{};
        dinfo("Kemu vCPU affinity: policy=system (initialization failed)");
    }
}

void WindowsAffinity::apply(HANDLE thread, unsigned vcpu) const
{
    if (mPlan.policy == Policy::System || mPlan.ideals.empty()) {
        return;
    }
    bool idealChanged = false;
    PROCESSOR_NUMBER previousIdeal{};
    const char* failure = nullptr;
    try {
        require(vcpu < mCount, "vCPU index exceeds startup configuration");
        // A component's explicit CPU Sets take precedence over our ideal hint.
        const auto read = reinterpret_cast<WindowsApi::ReadSets>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadSelectedCpuSets"));
        require(read && readSets(read, thread).empty(), "thread already has selected CPU Sets");
        GROUP_AFFINITY affinity{};
        require(GetThreadGroupAffinity(thread, &affinity) != FALSE,
                "cannot read thread affinity");
        std::string ideal = "system";
        {
            const auto& cpu = mPlan.ideals[vcpu % mPlan.ideals.size()];
            require(cpu.group == affinity.Group &&
                    (affinity.Mask & (KAFFINITY(1) << cpu.logical)),
                    "thread affinity restricts the ideal processor");
            PROCESSOR_NUMBER target{cpu.group, cpu.logical, 0}, actualIdeal{};
            require(SetThreadIdealProcessorEx(thread, &target, &previousIdeal) != FALSE,
                    "cannot set ideal processor");
            idealChanged = true;
            require(GetThreadIdealProcessorEx(thread, &actualIdeal) &&
                    actualIdeal.Group == target.Group && actualIdeal.Number == target.Number,
                    "ideal processor verification failed");
            ideal = std::to_string(cpu.logical);
        }
        dinfo("Kemu vCPU affinity: vcpu=%u tid=%lu policy=%s group=%u pool=%s ideal=%s",
                     vcpu, GetThreadId(thread), policyName(mPlan.policy),
                     unsigned(affinity.Group), mPool.c_str(), ideal.c_str());
        return;
    } catch (const AffinityError& error) {
        failure = error.reason;
    } catch (...) {
        failure = "thread assignment failed";
    }
    if (idealChanged && !SetThreadIdealProcessorEx(thread, &previousIdeal, nullptr)) {
        dinfo("Kemu vCPU affinity: vcpu=%u could not restore ideal processor", vcpu);
    }
    dinfo("Kemu vCPU affinity: vcpu=%u process pool retained (%s)", vcpu, failure);
}

} // namespace affinity
} // namespace kemu

extern "C" void kemu_process_affinity_init(unsigned count)
{
    std::call_once(initialized, [count] { runtime.initialize(count); });
}

extern "C" void kemu_vcpu_affinity_apply(unsigned vcpu, unsigned thread_id)
{
    if (runtime.plan().ideals.empty()) return;
    const auto thread = OpenThread(THREAD_QUERY_INFORMATION |
                                   THREAD_SET_INFORMATION, FALSE, thread_id);
    if (thread) {
        runtime.apply(thread, vcpu);
        CloseHandle(thread);
    } else {
        dinfo("Kemu vCPU affinity: cannot open vCPU thread %u", thread_id);
    }
}
