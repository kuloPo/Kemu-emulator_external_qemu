// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: MIT
#ifndef PIPE_SHARED_RECEIVE_PROTOCOL_H
#define PIPE_SHARED_RECEIVE_PROTOCOL_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __u32 PipeRxU32;
#else
#include <stdint.h>
typedef uint32_t PipeRxU32;
#endif

#define PIPE_RX_PAGE_SIZE 4096U
#define PIPE_RX_SEGMENT_SIZE 65536U
#define PIPE_RX_SEGMENT_PAGES 16U
#define PIPE_RX_INITIAL_SEGMENTS 4U
#define PIPE_RX_MAX_SEGMENTS 64U
#define PIPE_RX_MAPPING_SIZE ((2U + PIPE_RX_MAX_SEGMENTS * PIPE_RX_SEGMENT_PAGES) * PIPE_RX_PAGE_SIZE)
#define PIPE_RX_INVALID_SEGMENT 0xffffffffU
#define PIPE_RX_SEALED 0x80000000U
#define PIPE_RX_NEED_SEGMENT 1U
#define PIPE_RX_ERROR 2U
#define PIPE_RX_CLOSED 4U

#define PIPE_CMD_RX_SETUP 13
#define PIPE_CMD_RX_ADD_SEGMENT 14
#define PIPE_CMD_RX_ACTIVATE 15
#define PIPE_CMD_RX_CREDIT 16
#define PIPE_CMD_RX_ARM 17

struct PipeRxSegment {
    PipeRxU32 generation;
    PipeRxU32 committed;
    PipeRxU32 next;
    PipeRxU32 reserved;
};

struct PipeRxHost {
    PipeRxU32 event_seq;
    PipeRxU32 flags;
    PipeRxU32 write_segment;
    PipeRxU32 segment_count;
    PipeRxU32 irq_armed;
    PipeRxU32 reserved[11];
    struct PipeRxSegment segments[PIPE_RX_MAX_SEGMENTS];
};

struct PipeRxGuest {
    PipeRxU32 released[PIPE_RX_MAX_SEGMENTS];
};

// Include sys/ioctl.h or linux/ioctl.h before using these definitions.
#define PIPE_RX_INIT _IO('p', 0x81)
#define PIPE_RX_GROW _IO('p', 0x82)
#define PIPE_RX_CREDIT _IO('p', 0x83)
#define PIPE_RX_WAIT _IOW('p', 0x84, PipeRxU32)

#endif
