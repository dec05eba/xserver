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
#include <compositeext.h>

#include "driver.h"

#define MS_OVERLAY_MOVING_HOLD_USEC 250000

static drmmode_overlay_plane_rec *
ms_overlay_moving_plane(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return &drmmode_crtc->overlay_plane;
}

static drmmode_overlay_moving_rec *
ms_overlay_moving(xf86CrtcPtr crtc)
{
    return &ms_overlay_moving_plane(crtc)->moving;
}

static void
ms_overlay_moving_forget_fence(drmmode_overlay_moving_rec *moving)
{
    if (moving->fence_fd >= 0) {
        SetNotifyFd(moving->fence_fd, NULL, 0, NULL);
        close(moving->fence_fd);
    }
    moving->fence_fd = -1;
}

static void
ms_overlay_moving_painted(int fd, int xevents, void *data)
{
    SetNotifyFd(fd, NULL, 0, NULL);
    SetBlockHandlerDeadline(GetTimeInMicros());
}

static Bool
ms_overlay_moving_paint_done(drmmode_overlay_moving_rec *moving)
{
    if (moving->fence_fd >= 0 && !ms_fence_signaled(moving->fence_fd))
        return FALSE;

    ms_overlay_moving_forget_fence(moving);
    return TRUE;
}

static WindowPtr
ms_overlay_moving_candidate(xf86CrtcPtr crtc,
                            drmmode_overlay_position_rec *position, Bool *pq)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    WindowPtr window, plane_window;

    if (!ms_overlay_crtc_usable(crtc, pq))
        return NULL;

    window = CompositeServerMovingWindow(screen, GetTimeInMicros() - MS_OVERLAY_MOVING_HOLD_USEC);
    plane_window = CompositeServerPlaneWindow(screen);
    if (!window || (plane_window && plane_window != ms_overlay_moving(crtc)->window) ||
        (drmmode_crtc_output_expects_pq(crtc) && CompositeServerWindowTranslucent(window)) ||
        !ms_overlay_window_position(crtc, RegionExtents(&window->borderSize), position))
        return NULL;
    return window;
}

static Bool
ms_overlay_moving_paint(xf86CrtcPtr crtc, WindowPtr window, Bool pq)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_overlay_plane_rec *overlay = ms_overlay_moving_plane(crtc);
    drmmode_overlay_moving_rec *moving = &overlay->moving;
    int buffer;

    if (!ms_overlay_create_buffers(crtc))
        return FALSE;

    buffer = overlay->buffers.buf[0].fb_id == overlay->fb_id;
    if (!ms_compositor_paint_window(crtc, overlay->buffers.buf[buffer].px, window, pq,
                                    &overlay->buffers.cover_sdr))
        return FALSE;

    ms_overlay_moving_forget_fence(moving);
    moving->fence_fd = ms->glamor.egl_create_fence_fd(screen);
    if (moving->fence_fd < 0)
        ms->glamor.finish(screen);
    else if (!ms_fence_signaled(moving->fence_fd))
        SetNotifyFd(moving->fence_fd, ms_overlay_moving_painted, X_NOTIFY_READ, NULL);

    moving->buffer = buffer;
    moving->serial = CompositeServerWindowSerial(window);
    moving->pq = pq;
    return TRUE;
}

static uint32_t
ms_overlay_moving_ready_fb(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_moving_plane(crtc);
    drmmode_overlay_moving_rec *moving = &overlay->moving;

    if (moving->buffer < 0 || !ms_overlay_moving_paint_done(moving))
        return overlay->fb_id;

    return overlay->buffers.buf[moving->buffer].fb_id;
}

static void
ms_overlay_moving_demote(xf86CrtcPtr crtc)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    drmmode_overlay_moving_rec *moving = ms_overlay_moving(crtc);

    if (CompositeServerPlaneWindow(screen) == moving->window)
        CompositeServerSetPlaneWindow(screen, NULL);
    moving->window = NULL;
}

static void
ms_overlay_moving_promote(xf86CrtcPtr crtc, WindowPtr window,
                          const drmmode_overlay_position_rec *position, Bool pq)
{
    drmmode_overlay_moving_rec *moving = ms_overlay_moving(crtc);

    if (!ms_overlay_owner_settled(crtc, DRMMODE_OVERLAY_MOVING) ||
        ms_overlay_commit_in_flight(crtc) || !ms_overlay_moving_paint(crtc, window, pq))
        return;

    moving->window = window;
    moving->position = *position;
    CompositeServerSetPlaneWindow(xf86ScrnToScreen(crtc->scrn), window);
}

void
ms_overlay_moving_update(xf86CrtcPtr crtc)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    drmmode_overlay_moving_rec *moving = ms_overlay_moving(crtc);
    drmmode_overlay_position_rec position;
    WindowPtr window;
    Bool pq;

    if (moving->window && CompositeServerPlaneWindow(screen) != moving->window)
        moving->window = NULL;

    window = ms_overlay_moving_candidate(crtc, &position, &pq);
    if (moving->window && window != moving->window)
        ms_overlay_moving_demote(crtc);

    if (!window)
        return;

    if (!moving->window) {
        ms_overlay_moving_promote(crtc, window, &position, pq);
        return;
    }

    moving->position = position;
    if ((CompositeServerWindowSerial(window) != moving->serial || pq != moving->pq) &&
        !ms_overlay_commit_in_flight(crtc) && ms_overlay_moving_paint_done(moving))
        ms_overlay_moving_paint(crtc, window, pq);
}

Bool
ms_overlay_moving_active(xf86CrtcPtr crtc)
{
    return ms_overlay_moving(crtc)->window != NULL;
}

Bool
ms_overlay_moving_ready(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_moving_plane(crtc);

    return !overlay->plane_id || overlay->moving.buffer < 0 ||
        ms_overlay_moving_ready_fb(crtc) != 0;
}

void
ms_overlay_moving_fill(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_moving_plane(crtc);

    overlay->next_fb_id = ms_overlay_moving_ready_fb(crtc);
    overlay->next_position = overlay->moving.position;
}

Bool
ms_overlay_moving_pending(xf86CrtcPtr crtc, uint32_t *fb_id,
                          drmmode_overlay_position_rec *position)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_moving_plane(crtc);
    drmmode_overlay_moving_rec *moving = &overlay->moving;

    if (!moving->window)
        return FALSE;

    *fb_id = ms_overlay_moving_ready_fb(crtc);
    *position = moving->position;
    return *fb_id != overlay->fb_id ||
        !ms_overlay_position_equal(position, &overlay->position);
}

void
ms_overlay_moving_destroy(xf86CrtcPtr crtc)
{
    drmmode_overlay_moving_rec *moving = ms_overlay_moving(crtc);

    if (moving->window)
        ms_overlay_moving_demote(crtc);
    ms_overlay_moving_forget_fence(moving);
    moving->buffer = -1;
}
