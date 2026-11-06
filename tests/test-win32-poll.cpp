/* SPDX-License-Identifier: GPL-2.0-only */
#include "qemu/win32-poll.h"
#include <gtest/gtest.h>

TEST(Win32Poll, RequestsSupportedWinsockEvents) {
    EXPECT_EQ(POLLRDNORM, qemu_win32_poll_events(G_IO_IN));
    EXPECT_EQ(POLLWRNORM, qemu_win32_poll_events(G_IO_OUT));
    EXPECT_EQ(POLLRDBAND, qemu_win32_poll_events(G_IO_PRI));
    EXPECT_EQ(POLLRDNORM | POLLWRNORM | POLLRDBAND,
              qemu_win32_poll_events(G_IO_IN | G_IO_OUT | G_IO_PRI |
                                     G_IO_ERR | G_IO_HUP | G_IO_NVAL));
}

TEST(Win32Poll, ReportsReadinessAndExceptionalConditions) {
    const struct {
        short returned;
        int requested;
        int expected;
    } cases[] = {
        {0, G_IO_IN | G_IO_OUT, 0},
        {POLLRDNORM, G_IO_IN, G_IO_IN},
        {POLLRDNORM, G_IO_OUT, 0},
        {POLLWRNORM, G_IO_OUT, G_IO_OUT},
        {POLLRDBAND, G_IO_IN | G_IO_PRI, G_IO_PRI},
        {POLLPRI, G_IO_PRI, G_IO_PRI},
        {POLLRDBAND, G_IO_IN, 0},
        {POLLHUP, G_IO_IN | G_IO_PRI, G_IO_IN | G_IO_HUP},
        {POLLERR, G_IO_IN | G_IO_PRI, G_IO_IN | G_IO_ERR},
        {POLLRDNORM | POLLHUP, G_IO_IN | G_IO_PRI, G_IO_IN | G_IO_HUP},
        {POLLERR | POLLHUP, G_IO_OUT | G_IO_ERR, G_IO_ERR | G_IO_HUP},
        {POLLERR | POLLHUP | POLLWRNORM, G_IO_OUT | G_IO_ERR,
         G_IO_OUT | G_IO_ERR | G_IO_HUP},
        {POLLERR, 0, G_IO_ERR},
        {POLLHUP, 0, G_IO_HUP},
        {POLLNVAL, G_IO_IN | G_IO_OUT, G_IO_NVAL},
    };

    for (const auto& test : cases) {
        EXPECT_EQ(test.expected,
                  qemu_win32_poll_revents(test.returned, test.requested))
            << "revents=" << test.returned << " events=" << test.requested;
    }
}
