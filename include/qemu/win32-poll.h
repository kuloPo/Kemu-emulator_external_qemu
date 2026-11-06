/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QEMU_WIN32_POLL_H
#define QEMU_WIN32_POLL_H

#include <winsock2.h>
#include <glib.h>

static inline short qemu_win32_poll_events(int events)
{
    short result = 0;

    if (events & G_IO_IN) {
        result |= POLLRDNORM;
    }
    if (events & G_IO_OUT) {
        result |= POLLWRNORM;
    }
    if (events & G_IO_PRI) {
        result |= POLLRDBAND;
    }
    return result;
}

static inline int qemu_win32_poll_revents(short revents, int events)
{
    int result = 0;

    /* Keep EOF and errors readable for callers that only request G_IO_IN. */
    if (revents & (POLLRDNORM | POLLERR | POLLHUP)) {
        result |= G_IO_IN;
    }
    if (revents & POLLWRNORM) {
        result |= G_IO_OUT;
    }
    if (revents & (POLLPRI | POLLRDBAND)) {
        result |= G_IO_PRI;
    }
    if (revents & POLLERR) {
        result |= G_IO_ERR;
    }
    if (revents & POLLHUP) {
        result |= G_IO_HUP;
    }
    if (revents & POLLNVAL) {
        result |= G_IO_NVAL;
    }

    /* Like poll(), report exceptional conditions even if not requested. */
    return result & (events | G_IO_ERR | G_IO_HUP | G_IO_NVAL);
}

#endif
