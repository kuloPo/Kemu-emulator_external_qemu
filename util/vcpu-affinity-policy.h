/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KEMU_VCPU_AFFINITY_POLICY_H
#define KEMU_VCPU_AFFINITY_POLICY_H

#include <cstdint>
#include <vector>

namespace kemu {
namespace affinity {

constexpr uint32_t unknownTopology = UINT32_MAX;

struct Cpu {
    uint32_t id;
    uint16_t group;
    uint8_t logical;
    uint8_t efficiency;
    uint32_t core;
    uint32_t cache;
};

enum class Policy { System, Performance, Mixed, Cache };

struct Plan {
    Policy policy = Policy::System;
    std::vector<Cpu> pool;
    std::vector<Cpu> ideals;
};

/* Input contains only processors the process is allowed to use. */
Plan makePlan(std::vector<Cpu> cpus, unsigned vcpus);
const char* policyName(Policy policy);

} // namespace affinity
} // namespace kemu
#endif
