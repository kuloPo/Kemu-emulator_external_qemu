// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The channel owns the event. Unregister the doorbell before closing it. */
void* android_pipe_guest_write_event(void* pipe);

#ifdef __cplusplus
}
#endif
