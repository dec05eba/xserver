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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "dix-config.h"

#include <stdio.h>
#include <string.h>
#include <xf86.h>

#include "client.h"
#include "driver.h"

#if defined(HAVE_GBM) && defined(DRI3)
#include "dri3.h"

/* NVIDIA's PRIME render offload clients can only be recognized by the
 * environment they were started with.
 */
static Bool
ms_offload_renders_client(ScreenPtr screen, ClientPtr client)
{
    pid_t pid = GetClientPid(client);
    char path[32];
    char *entry = NULL;
    size_t size = 0;
    Bool offloaded = FALSE;
    FILE *file;

    if (pid <= 0)
        return FALSE;

    snprintf(path, sizeof(path), "/proc/%d/environ", (int) pid);
    file = fopen(path, "re");
    if (!file)
        return FALSE;

    while (!offloaded && getdelim(&entry, &size, '\0', file) > 0)
        offloaded = strcmp(entry, "__NV_PRIME_RENDER_OFFLOAD=1") == 0;

    free(entry);
    fclose(file);
    return offloaded;
}

static const dri3_offload_info_rec ms_offload_info = {
    .renders_client = ms_offload_renders_client,
    .can_scanout_window = ms_compositor_can_scanout_prime,
};

void
ms_offload_init(ScreenPtr screen)
{
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(screen));

    if (drmmode_driver_is(ms->fd, "nvidia-drm"))
        dri3_offload_init(screen, &ms_offload_info);
}

PixmapPtr
ms_offload_native_pixmap(PixmapPtr pixmap)
{
    return dri3_offload_native_pixmap(pixmap);
}
#elif defined(HAVE_GBM)
void
ms_offload_init(ScreenPtr screen)
{
}

PixmapPtr
ms_offload_native_pixmap(PixmapPtr pixmap)
{
    return NULL;
}
#endif
