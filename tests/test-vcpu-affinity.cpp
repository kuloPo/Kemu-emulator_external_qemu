/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include "../util/vcpu-affinity-policy.h"
#include "qemu/vcpu-affinity.h"
#include "../util/vcpu-affinity-win32.h"
#include "../android/android-emu-base/android/base/system/Win32ChildProcess.h"
#include <gtest/gtest.h>

#define CHECK(condition) ASSERT_TRUE(condition)

using namespace kemu::affinity;

static std::vector<unsigned> logicals(const std::vector<Cpu>& cpus)
{
    std::vector<unsigned> result;
    for (const auto& cpu : cpus) {
        result.push_back(cpu.logical);
    }
    return result;
}

static void policyTests()
{
    std::vector<Cpu> dualCache;
    for (unsigned i = 0; i < 32; ++i) {
        dualCache.push_back({1000 + i, 0, uint8_t(i), 0, i / 2, i / 16});
    }
    auto plan = makePlan(dualCache, 8);
    CHECK(plan.policy == Policy::Cache && plan.pool.size() == 16);
    CHECK((logicals(plan.ideals) == std::vector<unsigned>{
           0,2,4,6,8,10,12,14,1,3,5,7,9,11,13,15}));
    CHECK(makePlan(dualCache, 16).pool.size() == 16);
    plan = makePlan(dualCache, 18);
    CHECK(plan.pool.size() == 18);
    CHECK(plan.pool[16].logical == 16 && plan.pool[17].logical == 18);
    CHECK(makePlan(dualCache, 48).pool.size() == 32);
    std::reverse(dualCache.begin(), dualCache.end());
    CHECK(logicals(makePlan(dualCache, 18).pool) == logicals(plan.pool));

    /* SMT siblings need not be adjacent, even-numbered, or have consecutive IDs. */
    const std::vector<Cpu> sparse{
        {99,0,21,0,11,7}, {5,0,7,0,4,7}, {12,0,2,0,11,7},
        {7,0,30,0,4,7}, {18,0,31,0,9,3}, {19,0,5,0,9,3}};
    plan = makePlan(sparse, 5);
    CHECK((logicals(plan.pool) == std::vector<unsigned>{2,7,21,30,5}));
    CHECK((logicals(plan.ideals) == std::vector<unsigned>{2,7,21,30,5}));

    auto hybrid = std::vector<Cpu>{};
    for (unsigned i = 0; i < 16; ++i) {
        hybrid.push_back({400 + i, 0, uint8_t(i), 9, i / 2, unknownTopology});
    }
    for (unsigned i = 16; i < 24; ++i) {
        hybrid.push_back({400 + i, 0, uint8_t(i), 2, i, unknownTopology});
    }
    plan = makePlan(hybrid, 8);
    CHECK(plan.policy == Policy::Performance && plan.pool.size() == 16);
    CHECK(plan.ideals[7].logical == 14);
    for (unsigned n : {9u, 12u, 16u, 32u}) {
        plan = makePlan(hybrid, n);
        CHECK(plan.policy == Policy::Mixed && plan.pool.size() == 24);
        CHECK(plan.ideals.empty());
    }
    /* Newer P cores without SMT, three performance classes, nonzero group. */
    const std::vector<Cpu> noSmt{
        {20,2,5,7,50,1}, {21,2,1,7,51,1}, {22,2,9,3,52,1}, {23,2,20,1,53,2}};
    CHECK(makePlan(noSmt, 2).pool.size() == 2);
    CHECK(makePlan(noSmt, 3).policy == Policy::Mixed);

    /* Process affinity may expose only the second cache or one SMT sibling. */
    std::vector<Cpu> restricted;
    for (const auto& cpu : dualCache) {
        if (cpu.logical >= 16 && cpu.logical % 2) {
            restricted.push_back(cpu);
        }
    }
    plan = makePlan(restricted, 8);
    CHECK(plan.pool.size() == 8 && plan.ideals.front().logical == 17);
    CHECK(makePlan({}, 8).policy == Policy::System);
    CHECK(makePlan(dualCache, 0).policy == Policy::System);
    auto invalid = dualCache;
    invalid[0].cache = unknownTopology;
    CHECK(makePlan(invalid, 8).policy == Policy::System);
    invalid = hybrid;
    invalid[0].core = unknownTopology;
    CHECK(makePlan(invalid, 8).policy == Policy::System);
    invalid = dualCache;
    invalid[0].group = 1;
    CHECK(makePlan(invalid, 8).policy == Policy::System);
    invalid = dualCache;
    invalid[0].id = invalid[1].id;
    CHECK(makePlan(invalid, 8).policy == Policy::System);
    invalid = dualCache;
    invalid[0].logical = invalid[1].logical;
    CHECK(makePlan(invalid, 8).policy == Policy::System);
    std::puts("Affinity policy tests passed.");
}

TEST(Policy, Topologies) { policyTests(); }

namespace {
std::string scenario;
using ReadSets = WindowsApi::ReadSets;
using SelectSets = BOOL (WINAPI*)(HANDLE, const ULONG*, ULONG);
std::vector<ULONG> selected(HANDLE thread) {
    const auto read = reinterpret_cast<ReadSets>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadSelectedCpuSets"));
    if (!read) return {};
    ULONG count = 0;
    read(thread, nullptr, 0, &count);
    std::vector<ULONG> ids(count);
    if (count && !read(thread, ids.data(), count, &count)) ADD_FAILURE() << "Cannot read CPU Sets";
    return ids;
}
struct RestoreProcess {
    DWORD_PTR mask = 0, system = 0;
    RestoreProcess() { GetProcessAffinityMask(GetCurrentProcess(), &mask, &system); }
    ~RestoreProcess() {
        android::base::childprocess::setAffinity(0, 0);
        SetProcessAffinityMask(GetCurrentProcess(), mask);
    }
};
DWORD_PTR processMask() {
    DWORD_PTR mask = 0, system = 0;
    EXPECT_TRUE(GetProcessAffinityMask(GetCurrentProcess(), &mask, &system));
    return mask;
}
DWORD_PTR firstBit(DWORD_PTR value) { return value & (~value + 1); }
void checkWorker(DWORD_PTR pool) {
    GROUP_AFFINITY group{};
    ASSERT_TRUE(GetThreadGroupAffinity(GetCurrentThread(), &group));
    EXPECT_NE(group.Mask, 0u);
    EXPECT_EQ(group.Mask & ~pool, 0u);
    for (int sample = 0; sample < 64; ++sample) {
        PROCESSOR_NUMBER current{};
        GetCurrentProcessorNumberEx(&current);
        EXPECT_EQ(current.Group, group.Group);
        EXPECT_NE(pool & (DWORD_PTR(1) << current.Number), 0u);
        SwitchToThread();
    }
}
}

TEST(Native, Workers) {
    RestoreProcess restore;
    if (GetActiveProcessorGroupCount() != 1) GTEST_SKIP() << "Native pool assertions require one group";
    const auto api = systemApi();
    if (!api.query || !api.defaults) GTEST_SKIP() << "CPU Set APIs unavailable";
    unsigned count = scenario == "16" ? 16 : scenario == "18" ? 18 : 8;
    if (scenario == "restrict") {
        DWORD_PTR mask = 0;
        bool take = true;
        for (unsigned bit = 0; bit < sizeof(mask) * 8; ++bit) {
            if (restore.mask & (DWORD_PTR(1) << bit)) {
                if (take) mask |= DWORD_PTR(1) << bit;
                take = !take;
            }
        }
        ASSERT_TRUE(SetProcessAffinityMask(GetCurrentProcess(), mask));
    }
    const auto before = processMask();
    if (!SetProcessAffinityMask(GetCurrentProcess(), before))
        GTEST_SKIP() << "Host does not permit process affinity changes";
    ULONG size = 0;
    api.query(nullptr, 0, &size, GetCurrentProcess(), 0);
    std::vector<unsigned char> data(size);
    ASSERT_TRUE(api.query(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(data.data()), size, &size, GetCurrentProcess(), 0));
    std::vector<Cpu> cpus;
    for (size_t at = 0; at < size;) {
        const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(data.data() + at);
        ASSERT_GT(entry->Size, 0u);
        if (entry->Type == CpuSetInformation) {
            const auto& c = entry->CpuSet;
            if (c.Group == 0 && (before & (DWORD_PTR(1) << c.LogicalProcessorIndex)) && (!c.Allocated || c.AllocatedToTargetProcess))
                cpus.push_back({c.Id, c.Group, c.LogicalProcessorIndex, c.EfficiencyClass, unknownTopology, unknownTopology});
        }
        at += entry->Size;
    }
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &bytes);
    std::vector<unsigned char> relationships(bytes);
    ASSERT_TRUE(GetLogicalProcessorInformationEx(RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(relationships.data()), &bytes));
    uint32_t core = 0, cache = 0;
    for (size_t at = 0; at < bytes;) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(relationships.data() + at);
        const GROUP_AFFINITY* masks = nullptr;
        unsigned masksCount = 0;
        if (entry->Relationship == RelationProcessorCore) {
            masks = entry->Processor.GroupMask; masksCount = entry->Processor.GroupCount; ++core;
        } else if (entry->Relationship == RelationCache && entry->Cache.Level == 3 && entry->Cache.Type == CacheUnified) {
            masks = &entry->Cache.GroupMask; masksCount = std::max(1u, unsigned(entry->Cache.GroupCount)); ++cache;
        }
        for (auto& cpu : cpus) for (unsigned n = 0; n < masksCount; ++n) {
            if (cpu.group == masks[n].Group && (masks[n].Mask & (DWORD_PTR(1) << cpu.logical))) {
                if (entry->Relationship == RelationProcessorCore) cpu.core = core;
                else cpu.cache = cache;
            }
        }
        ASSERT_GT(entry->Size, 0u);
        at += entry->Size;
    }
    ASSERT_FALSE(cpus.empty());
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto setDefaults = reinterpret_cast<SelectSets>(GetProcAddress(kernel, "SetProcessDefaultCpuSets"));
    const auto setSelected = reinterpret_cast<SelectSets>(GetProcAddress(kernel, "SetThreadSelectedCpuSets"));
    ASSERT_TRUE(setDefaults && setSelected);
    ULONG defaultsCount = 0;
    api.defaults(GetCurrentProcess(), nullptr, 0, &defaultsCount);
    std::vector<ULONG> defaults(defaultsCount);
    if (defaultsCount) ASSERT_TRUE(api.defaults(GetCurrentProcess(), defaults.data(), defaultsCount, &defaultsCount));
    if (scenario == "defaults") {
        defaults = {cpus.front().id};
        ASSERT_TRUE(setDefaults(GetCurrentProcess(), defaults.data(), ULONG(defaults.size())));
    }
    if (!defaults.empty()) cpus.erase(std::remove_if(cpus.begin(), cpus.end(), [&](const Cpu& cpu) {
        return std::find(defaults.begin(), defaults.end(), cpu.id) == defaults.end();
    }), cpus.end());
    auto expected = makePlan(cpus, count);
    if (scenario == "system" || scenario == "invalid") {
        _putenv_s("KEMU_VCPU_AFFINITY", scenario.c_str());
        expected = {};
    } else _putenv_s("KEMU_VCPU_AFFINITY", "auto");
    std::atomic<bool> ready{false}, release{false};
    DWORD_PTR pool = before;
    std::thread existing([&] { ready = true; while (!release) SwitchToThread(); checkWorker(pool); });
    while (!ready) SwitchToThread();
    WindowsAffinity affinity;
    affinity.initialize(count);
    EXPECT_EQ(affinity.plan().policy, expected.policy);
    pool = processMask();
    DWORD_PTR expectedMask = 0;
    for (const auto& cpu : expected.pool) expectedMask |= DWORD_PTR(1) << cpu.logical;
    EXPECT_EQ(pool, expected.policy == Policy::System ? before : expectedMask);
    release = true;
    existing.join();
    affinity.initialize(count + 1);
    EXPECT_EQ(processMask(), pool);
    std::vector<std::thread> workers;
    for (unsigned n = 0; n < count; ++n) workers.emplace_back([&, n] {
        std::vector<ULONG> overrideIds;
        PROCESSOR_NUMBER oldIdeal{};
        ASSERT_TRUE(GetThreadIdealProcessorEx(GetCurrentThread(), &oldIdeal));
        if (scenario == "thread-override") {
            overrideIds.push_back(cpus.front().id);
            ASSERT_TRUE(setSelected(GetCurrentThread(), overrideIds.data(), 1));
        }
        if (scenario == "pinned") ASSERT_NE(SetThreadAffinityMask(GetCurrentThread(), firstBit(pool)), 0u);
        affinity.apply(GetCurrentThread(), n);
        EXPECT_EQ(selected(GetCurrentThread()), overrideIds);
        GROUP_AFFINITY group{};
        ASSERT_TRUE(GetThreadGroupAffinity(GetCurrentThread(), &group));
        if (scenario == "pinned") EXPECT_EQ(group.Mask, firstBit(pool));
        PROCESSOR_NUMBER ideal{};
        ASSERT_TRUE(GetThreadIdealProcessorEx(GetCurrentThread(), &ideal));
        if (!expected.ideals.empty() && overrideIds.empty() && scenario != "pinned") {
            EXPECT_EQ(ideal.Group, expected.ideals[n % expected.ideals.size()].group);
            EXPECT_EQ(ideal.Number, expected.ideals[n % expected.ideals.size()].logical);
        } else if (!overrideIds.empty()) {
            EXPECT_EQ(ideal.Group, oldIdeal.Group); EXPECT_EQ(ideal.Number, oldIdeal.Number);
        }
        checkWorker(pool);
    });
    for (auto& worker : workers) worker.join();
    std::thread newHost([&] { checkWorker(pool); }); newHost.join();
    // A later process-default change remains effective without a thread override.
    const ULONG id = cpus.front().id;
    ASSERT_TRUE(setDefaults(GetCurrentProcess(), &id, 1));
    std::thread followsDefaults([&] {
        affinity.apply(GetCurrentThread(), 0);
        EXPECT_TRUE(selected(GetCurrentThread()).empty());
    });
    followsDefaults.join();
    ASSERT_TRUE(setDefaults(GetCurrentProcess(), nullptr, 0));
}

namespace {
DWORD_PTR fixtureMask = 0xffffffff;
bool missingCache = false, missingCore = false, failSet = false, badReadback = false;
unsigned sets = 0;
BOOL WINAPI fixtureGet(HANDLE, PDWORD_PTR mask, PDWORD_PTR system) {
    *mask = fixtureMask; *system = 0xffffffff;
    if (badReadback && sets == 1) { *mask ^= 1; badReadback = false; }
    return TRUE;
}
BOOL WINAPI fixtureSet(HANDLE, DWORD_PTR mask) {
    ++sets;
    if (failSet) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    fixtureMask = mask; return TRUE;
}
WORD WINAPI fixtureGroups() { return 1; }
WORD WINAPI multipleGroups() { return 2; }
BOOL WINAPI fixtureDefaults(HANDLE, PULONG, ULONG, PULONG count) { *count = 0; return TRUE; }
BOOL WINAPI fixtureQuery(PSYSTEM_CPU_SET_INFORMATION output, ULONG size, PULONG required, HANDLE, ULONG) {
    *required = 32 * sizeof(SYSTEM_CPU_SET_INFORMATION);
    if (size < *required) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (unsigned n = 0; n < 32; ++n) {
        output[n] = {}; output[n].Size = sizeof(output[n]); output[n].Type = CpuSetInformation;
        output[n].CpuSet.Id = 1000 + n; output[n].CpuSet.LogicalProcessorIndex = BYTE(n);
    }
    return TRUE;
}
BOOL WINAPI fixtureRelationships(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX output, PDWORD size) {
    const DWORD required = 18 * sizeof(*output);
    if (*size < required) { *size = required; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *size = required;
    for (unsigned n = 0; n < 18; ++n) {
        output[n] = {}; output[n].Size = sizeof(*output);
        if (n < 16) {
            output[n].Relationship = RelationProcessorCore;
            output[n].Processor.GroupCount = 1;
            output[n].Processor.GroupMask[0].Mask = missingCore && n == 0 ? 0 : DWORD_PTR(3) << (n * 2);
        } else {
            output[n].Relationship = RelationCache;
            output[n].Cache.Level = missingCache ? 2 : 3;
            output[n].Cache.Type = CacheUnified;
            output[n].Cache.GroupMask.Mask = DWORD_PTR(0xffff) << ((n - 16) * 16);
        }
    }
    return TRUE;
}
WindowsApi fixtureApi() {
    _putenv_s("KEMU_VCPU_AFFINITY", "auto");
    fixtureMask = 0xffffffff; sets = 0;
    missingCache = missingCore = failSet = badReadback = false;
    WindowsApi api;
    api.query = fixtureQuery; api.defaults = fixtureDefaults;
    api.relationships = fixtureRelationships; api.getMask = fixtureGet;
    api.setMask = fixtureSet; api.groups = fixtureGroups;
    return api;
}
void expectFallback(const WindowsApi& api) {
    WindowsAffinity affinity; affinity.initialize(8, api);
    EXPECT_EQ(affinity.plan().policy, Policy::System);
    EXPECT_EQ(fixtureMask, DWORD_PTR(0xffffffff));
}
}
TEST(WindowsAffinity, MissingCache) { auto api = fixtureApi(); missingCache = true; expectFallback(api); EXPECT_EQ(sets, 0u); }
TEST(WindowsAffinity, MissingCore) { auto api = fixtureApi(); missingCore = true; expectFallback(api); EXPECT_EQ(sets, 0u); }
TEST(WindowsAffinity, MissingApi) { auto api = fixtureApi(); api.query = nullptr; expectFallback(api); EXPECT_EQ(sets, 0u); }
TEST(WindowsAffinity, MultiGroup) { auto api = fixtureApi(); api.groups = multipleGroups; expectFallback(api); EXPECT_EQ(sets, 0u); }
TEST(WindowsAffinity, SetFailure) { auto api = fixtureApi(); failSet = true; expectFallback(api); EXPECT_EQ(sets, 1u); }
TEST(WindowsAffinity, ReadbackFailure) { auto api = fixtureApi(); badReadback = true; expectFallback(api); EXPECT_EQ(sets, 2u); }

static void expectChildMask(DWORD_PTR expected) {
    wchar_t path[MAX_PATH]{}; ASSERT_NE(GetSystemDirectoryW(path, MAX_PATH), 0u);
    const std::wstring program = std::wstring(path) + L"\\cmd.exe";
    std::wstring command = L"cmd.exe /c exit 0";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
    ASSERT_TRUE(android::base::childprocess::create(program.c_str(), command.data(), nullptr, nullptr,
        FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child));
    DWORD_PTR mask = 0, system = 0;
    EXPECT_TRUE(GetProcessAffinityMask(child.hProcess, &mask, &system));
    EXPECT_EQ(mask, expected);
    EXPECT_NE(ResumeThread(child.hThread), DWORD(-1));
    if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) {
        ADD_FAILURE() << "Child did not exit"; TerminateProcess(child.hProcess, 1);
    }
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
}
TEST(WindowsAffinity, ChildInheritance) {
    RestoreProcess restore;
    if (GetActiveProcessorGroupCount() != 1) GTEST_SKIP();
    const auto pool = firstBit(restore.mask);
    ASSERT_TRUE(SetProcessAffinityMask(GetCurrentProcess(), pool));
    android::base::childprocess::setAffinity(restore.mask, pool);
    expectChildMask(restore.mask);
    EXPECT_EQ(processMask(), pool);
}
TEST(WindowsAffinity, ExternalRestriction) {
    RestoreProcess restore;
    if (GetActiveProcessorGroupCount() != 1 || restore.mask == firstBit(restore.mask)) GTEST_SKIP();
    const auto baseline = restore.mask & ~firstBit(restore.mask);
    const auto pool = firstBit(baseline);
    ASSERT_TRUE(SetProcessAffinityMask(GetCurrentProcess(), pool));
    android::base::childprocess::setAffinity(baseline, pool);
    expectChildMask(baseline);
    const auto external = firstBit(restore.mask);
    ASSERT_TRUE(SetProcessAffinityMask(GetCurrentProcess(), external));
    // A disjoint external restriction must never be widened to the old baseline.
    EXPECT_EQ(android::base::childprocess::childMask(), 0u);
    expectChildMask(external);
}
TEST(WindowsAffinity, JobRestriction) {
    RestoreProcess restore;
    if (GetActiveProcessorGroupCount() != 1 || restore.mask == firstBit(restore.mask)) GTEST_SKIP();
    const auto baseline = restore.mask & ~firstBit(restore.mask);
    HANDLE job = CreateJobObjectW(nullptr, nullptr); ASSERT_TRUE(job);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_AFFINITY;
    limit.BasicLimitInformation.Affinity = baseline;
    ASSERT_TRUE(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit)));
    ASSERT_TRUE(AssignProcessToJobObject(job, GetCurrentProcess()));
    const auto pool = firstBit(baseline);
    ASSERT_TRUE(SetProcessAffinityMask(GetCurrentProcess(), pool));
    android::base::childprocess::setAffinity(baseline, pool);
    expectChildMask(baseline);
    CloseHandle(job);
}

int main(int argc, char** argv) {
    bool filtered = false;
    for (int n = 1; n < argc; ++n) {
        const std::string argument = argv[n];
        if (argument.find("--gtest_filter=") == 0) filtered = true;
        if (argument == "--native" && n + 1 < argc) scenario = argv[++n];
    }
    ::testing::InitGoogleTest(&argc, argv);
    if (!filtered) ::testing::GTEST_FLAG(filter) = scenario.empty() ? "Policy.*" : "Native.*";
    return RUN_ALL_TESTS();
}
