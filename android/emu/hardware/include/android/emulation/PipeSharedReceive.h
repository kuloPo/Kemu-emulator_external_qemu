// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "android/emulation/PipeSharedReceiveProtocol.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PipeRxDirty)(void* address, size_t length);
int android_pipe_rx_setup(void* pipe, struct PipeRxHost* host,
                          struct PipeRxGuest* guest, PipeRxDirty dirty, bool restoring,
                          void (*enter)(void), void (*leave)(void));
int android_pipe_rx_add_segment(void* pipe, unsigned id, void* const* pages);
int android_pipe_rx_activate(void* pipe);
void android_pipe_rx_credit(void* pipe);
void android_pipe_rx_set_running(void* pipe, bool running);
void android_pipe_rx_arm(void* pipe, uint32_t sequence);

#ifdef __cplusplus
}
#endif
