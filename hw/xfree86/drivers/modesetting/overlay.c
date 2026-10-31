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

#include <xf86.h>
#include <xf86Crtc.h>

#include "driver.h"

static drmmode_overlay_plane_rec *
ms_overlay_plane(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return &drmmode_crtc->overlay_plane;
}

static void
ms_overlay_destroy_buffers(xf86CrtcPtr crtc)
{
    drmmode_overlay_buffers_rec *buffers = &ms_overlay_plane(crtc)->buffers;
    int i;

    for (i = 0; i < ARRAY_SIZE(buffers->buf); i++) {
        if (buffers->buf[i].px)
            drmmode_shadow_fb_destroy(crtc, buffers->buf[i].px, (void *)(long)1,
                                      &buffers->buf[i].bo,
                                      &buffers->buf[i].fb_id);
        buffers->buf[i].px = NULL;
    }
    if (buffers->cover_sdr)
        dixDestroyPixmap(buffers->cover_sdr, 0);
    buffers->cover_sdr = NULL;
    buffers->back_idx = 0;
    buffers->pending = FALSE;
}

static Bool
ms_overlay_create_buffers(xf86CrtcPtr crtc)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);
    drmmode_overlay_buffers_rec *buffers = &overlay->buffers;
    int width = crtc->mode.HDisplay;
    int height = crtc->mode.VDisplay;
    int i;

    if (buffers->buf[0].px &&
        (buffers->buf[0].px->drawable.width != width ||
         buffers->buf[0].px->drawable.height != height)) {
        if (overlay->fb_id)
            return FALSE;
        ms_overlay_destroy_buffers(crtc);
    }

    for (i = 0; i < ARRAY_SIZE(buffers->buf); i++) {
        drmmode_shadow_fb_ptr buf = &buffers->buf[i];
        uint32_t fb_id;

        if (buf->px)
            continue;

        buf->px = drmmode_shadow_fb_create(crtc, NULL, width, height, 32,
                                           &buf->bo, &buf->fb_id);
        if (!buf->px)
            return FALSE;

        if (drmmode_bo_import_alpha(&ms->drmmode, &buf->bo, &fb_id)) {
            drmmode_shadow_fb_destroy(crtc, buf->px, (void *)(long)1,
                                      &buf->bo, &buf->fb_id);
            buf->px = NULL;
            return FALSE;
        }
        drmModeRmFB(ms->fd, buf->fb_id);
        buf->fb_id = fb_id;
    }
    return TRUE;
}

PixmapPtr
ms_overlay_back_buffer(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    if (!overlay->plane_id || !ms_overlay_create_buffers(crtc))
        return NULL;

    return overlay->buffers.buf[overlay->buffers.back_idx].px;
}

void
ms_overlay_queue(xf86CrtcPtr crtc, const BoxRec *box)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);
    drmmode_overlay_buffers_rec *buffers = &overlay->buffers;

    overlay->next_fb_id = buffers->buf[buffers->back_idx].fb_id;
    overlay->next_box = *box;
    buffers->pending = TRUE;
}

void
ms_overlay_flip_done(xf86CrtcPtr crtc)
{
    drmmode_overlay_buffers_rec *buffers = &ms_overlay_plane(crtc)->buffers;

    if (buffers->pending)
        buffers->back_idx ^= 1;
    buffers->pending = FALSE;
}

void
ms_overlay_flip_aborted(xf86CrtcPtr crtc)
{
    ms_overlay_plane(crtc)->buffers.pending = FALSE;
}

void
ms_overlay_flip_failed(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);
    Bool committed = !overlay->next_fb_id;

    if (!overlay->buffers.pending)
        return;

    overlay->buffers.pending = FALSE;
    overlay->next_fb_id = 0;
    if (!committed || overlay->fb_id)
        return;

    xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
               "Overlay plane %u was rejected, not using it\n",
               overlay->plane_id);
    overlay->plane_id = 0;
}

void
ms_overlay_destroy(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    ms_overlay_destroy_buffers(crtc);
    overlay->fb_id = 0;
    overlay->next_fb_id = 0;
}
