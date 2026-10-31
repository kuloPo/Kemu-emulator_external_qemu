// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: MIT
#ifndef PIPE_SHARED_TRANSMIT_PROTOCOL_H
#define PIPE_SHARED_TRANSMIT_PROTOCOL_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __u32 PipeTxU32;
#else
#include <stdint.h>
typedef uint32_t PipeTxU32;
#endif

#define PIPE_TX_PAGE_SIZE 4096U
#define PIPE_TX_SEGMENT_SIZE 65536U
#define PIPE_TX_INITIAL_SEGMENTS 4U
#define PIPE_TX_MAX_SEGMENTS 64U
#define PIPE_TX_MAX_PAGES 65536U
#define PIPE_TX_MMAP_BASE 65536U
#define PIPE_TX_INVALID_SEGMENT 0xffffffffU
#define PIPE_TX_SEALED 0x80000000U
#define PIPE_TX_ERROR 1U
#define PIPE_TX_CLOSED 2U

#define PIPE_CMD_TX_SETUP 19
#define PIPE_CMD_TX_ADD_PAGES 20
#define PIPE_CMD_TX_FINISH_SEGMENT 21
#define PIPE_CMD_TX_ACTIVATE 22
#define PIPE_CMD_TX_KICK 23
#define PIPE_CMD_TX_ARM 24

struct PipeTxSegment {
    PipeTxU32 generation;
    PipeTxU32 committed;
    PipeTxU32 next;
    PipeTxU32 reserved;
};

struct PipeTxGuest {
    struct PipeTxSegment segments[PIPE_TX_MAX_SEGMENTS];
};

struct PipeTxHost {
    PipeTxU32 event_seq;
    PipeTxU32 flags;
    PipeTxU32 read_segment;
    PipeTxU32 read_generation;
    PipeTxU32 read_offset;
    PipeTxU32 segment_count;
    PipeTxU32 irq_armed;
    PipeTxU32 reserved;
    PipeTxU32 capacities[PIPE_TX_MAX_SEGMENTS];
    PipeTxU32 released[PIPE_TX_MAX_SEGMENTS];
};

struct PipeTxAllocation {
    PipeTxU32 bytes;
    PipeTxU32 id;
};

// Include sys/ioctl.h or linux/ioctl.h before using these definitions.
#define PIPE_TX_INIT _IO('p', 0x85)
#define PIPE_TX_GROW _IOWR('p', 0x86, struct PipeTxAllocation)
#define PIPE_TX_ACTIVATE _IO('p', 0x87)
#define PIPE_TX_KICK _IO('p', 0x88)
#define PIPE_TX_WAIT _IOW('p', 0x89, PipeTxU32)

#endif
