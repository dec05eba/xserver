/*
 * Copyright © 2026 dec05eba
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <unistd.h>
#include <sys/timerfd.h>

#include "driver.h"

Bool
ms_timer_create(int *fd, NotifyFdProcPtr notify, void *data)
{
    if (*fd >= 0)
        return TRUE;

    *fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (*fd < 0)
        return FALSE;

    if (SetNotifyFd(*fd, notify, X_NOTIFY_READ, data))
        return TRUE;

    close(*fd);
    *fd = -1;
    return FALSE;
}

void
ms_timer_destroy(int *fd)
{
    if (*fd < 0)
        return;

    SetNotifyFd(*fd, NULL, 0, NULL);
    close(*fd);
    *fd = -1;
}

void
ms_timer_set(int fd, uint64_t *armed_at, uint64_t at)
{
    struct itimerspec spec = {
        .it_value = {
            .tv_sec = at / 1000000,
            .tv_nsec = at % 1000000 * 1000,
        },
    };

    if (*armed_at != at &&
        !timerfd_settime(fd, at ? TFD_TIMER_ABSTIME : 0, &spec, NULL))
        *armed_at = at;
}

Bool
ms_timer_expired(int fd)
{
    uint64_t expirations;

    return read(fd, &expirations, sizeof(expirations)) == sizeof(expirations);
}
