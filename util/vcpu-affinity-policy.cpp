/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "vcpu-affinity-policy.h"
#include <algorithm>
#include <set>
#include <utility>

namespace kemu {
namespace affinity {
namespace {
using Core = std::pair<uint16_t, uint32_t>;

std::vector<Cpu> physicalFirst(const std::vector<Cpu>& cpus)
{
    std::set<Core> seen;
    std::vector<Cpu> first, siblings;
    for (const auto& cpu : cpus) {
        (seen.insert({cpu.group, cpu.core}).second ? first : siblings)
            .push_back(cpu);
    }
    first.insert(first.end(), siblings.begin(), siblings.end());
    return first;
}

size_t coreCount(const std::vector<Cpu>& cpus)
{
    std::set<Core> cores;
    for (const auto& cpu : cpus) {
        cores.insert({cpu.group, cpu.core});
    }
    return cores.size();
}
} // namespace

Plan makePlan(std::vector<Cpu> cpus, unsigned vcpus)
{
    Plan plan;
    if (cpus.empty() || !vcpus) {
        return plan;
    }
    std::sort(cpus.begin(), cpus.end(), [](const Cpu& a, const Cpu& b) {
        return std::make_pair(a.group, a.logical) <
               std::make_pair(b.group, b.logical);
    });
    std::set<uint32_t> ids;
    std::set<std::pair<uint16_t, uint8_t>> processors;
    uint8_t low = cpus.front().efficiency, high = low;
    for (const auto& cpu : cpus) {
        /* Multi-group inputs retain system scheduling. */
        if (cpu.group != cpus.front().group || cpu.logical >= 64 ||
            cpu.core == unknownTopology || !ids.insert(cpu.id).second ||
            !processors.insert({cpu.group, cpu.logical}).second) {
            return plan;
        }
        low = std::min(low, cpu.efficiency);
        high = std::max(high, cpu.efficiency);
    }

    if (low != high) {
        std::vector<Cpu> performance;
        for (const auto& cpu : cpus) {
            if (cpu.efficiency == high) {
                performance.push_back(cpu);
            }
        }
        if (vcpus <= coreCount(performance)) {
            plan.policy = Policy::Performance;
            plan.pool = performance;
            plan.ideals = physicalFirst(performance);
        } else {
            plan.policy = Policy::Mixed;
            plan.pool = cpus;
            /* Do not permanently associate a guest vCPU with a slower core. */
        }
        return plan;
    }

    /* Group processors by shared L3 cache to preserve locality. */
    std::vector<uint32_t> domains;
    for (const auto& cpu : cpus) {
        if (cpu.cache == unknownTopology) {
            return plan;
        }
        if (std::find(domains.begin(), domains.end(), cpu.cache) == domains.end()) {
            domains.push_back(cpu.cache);
        }
    }
    plan.policy = Policy::Cache;
    for (const auto domain : domains) {
        std::vector<Cpu> members;
        for (const auto& cpu : cpus) {
            if (cpu.cache == domain) {
                members.push_back(cpu);
            }
        }
        const auto ordered = physicalFirst(members);
        if (plan.pool.empty()) {
            /* Keep all SMT siblings available within the preferred domain. */
            plan.pool = members;
            plan.ideals = ordered;
        } else {
            const size_t needed = std::min(size_t(vcpus) - plan.pool.size(),
                                           ordered.size());
            plan.pool.insert(plan.pool.end(), ordered.begin(), ordered.begin() + needed);
            plan.ideals.insert(plan.ideals.end(), ordered.begin(), ordered.begin() + needed);
        }
        if (plan.pool.size() >= vcpus) {
            break;
        }
    }
    return plan;
}

const char* policyName(Policy policy)
{
    switch (policy) {
    case Policy::Performance: return "performance";
    case Policy::Mixed: return "mixed";
    case Policy::Cache: return "l3-locality";
    default: return "system";
    }
}
} // namespace affinity
} // namespace kemu
