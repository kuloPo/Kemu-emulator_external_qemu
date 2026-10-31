// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "android/emulation/PipeSharedReceive.h"
#include "android/emulation/PipeSharedTransmitProtocol.h"

#ifdef __cplusplus
extern "C" {
#endif

int android_pipe_tx_setup(void* pipe, struct PipeTxGuest* guest,
                          struct PipeTxHost* host, PipeRxDirty dirty, bool restoring,
                          void (*enter)(void), void (*leave)(void));
int android_pipe_tx_add_segment(void* pipe, unsigned id, void* const* pages, unsigned count);
int android_pipe_tx_activate(void* pipe);
void android_pipe_tx_kick(void* pipe);
void android_pipe_tx_arm(void* pipe, uint32_t sequence);
void android_pipe_tx_detach(void* pipe);
void android_pipe_tx_set_running(void* pipe, bool running);

#ifdef __cplusplus
}
#endif
