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

#include <string.h>
#include <unistd.h>
#include <xf86.h>
#include <xf86Crtc.h>
#include <compositeext.h>

#include "driver.h"

/* amdgpu rejects planes that show less than 24x12 pixels */
#define MS_OVERLAY_MIN_WIDTH 24
#define MS_OVERLAY_MIN_HEIGHT 12

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
    ms_overlay_moving_destroy(crtc);
}

Bool
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

static void
ms_overlay_grow(INT16 *start, INT16 *end, int min_size, int bound_start,
                int bound_end)
{
    int size = min(max(*end - *start, min_size), bound_end - bound_start);

    *start = max(min(*start, bound_end - size), bound_start);
    *end = *start + size;
}

Bool
ms_overlay_size_supported(int width, int height)
{
    return width >= MS_OVERLAY_MIN_WIDTH && height >= MS_OVERLAY_MIN_HEIGHT;
}

Bool
ms_overlay_position_equal(const drmmode_overlay_position_rec *a,
                          const drmmode_overlay_position_rec *b)
{
    return a->src_x == b->src_x && a->src_y == b->src_y &&
        a->box.x1 == b->box.x1 && a->box.y1 == b->box.y1 &&
        a->box.x2 == b->box.x2 && a->box.y2 == b->box.y2;
}

Bool
ms_overlay_commit_in_flight(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return drmmode_crtc->tearfree.flip_seq || ms_cursor_in_flight(crtc);
}

Bool
ms_overlay_crtc_usable(xf86CrtcPtr crtc, Bool *pq)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

#ifdef HAVE_GBM
    if (ms_prime_crtc_flipping(crtc))
        return FALSE;
#endif

    return drmmode_crtc->overlay_plane.plane_id && !crtc->transform_in_use &&
        ms_tearfree_is_active_on_crtc(crtc) && !drmmode_crtc->tearfree.scanout.fb_id &&
        drmmode_crtc_overlay_content(crtc, pq) &&
        !CompositeServerHdrWindow(xf86ScrnToScreen(crtc->scrn), &crtc->bounds);
}

static Bool
ms_overlay_box_on_other_crtc(xf86CrtcPtr crtc, const BoxRec *box)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
    int i;

    for (i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr other = config->crtc[i];

        if (other != crtc && other->enabled &&
            box->x1 < other->bounds.x2 && other->bounds.x1 < box->x2 &&
            box->y1 < other->bounds.y2 && other->bounds.y1 < box->y2)
            return TRUE;
    }
    return FALSE;
}

Bool
ms_overlay_window_position(xf86CrtcPtr crtc, const BoxRec *win,
                           drmmode_overlay_position_rec *position)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    const BoxRec *bounds = &crtc->bounds;
    BoxRec visible = {
        .x1 = max(win->x1, bounds->x1),
        .y1 = max(win->y1, bounds->y1),
        .x2 = min(win->x2, bounds->x2),
        .y2 = min(win->y2, bounds->y2),
    };

    if (win->x2 - win->x1 > crtc->mode.HDisplay || win->y2 - win->y1 > crtc->mode.VDisplay ||
        visible.x2 <= visible.x1 || visible.y2 <= visible.y1 ||
        !ms_overlay_size_supported(visible.x2 - visible.x1, visible.y2 - visible.y1) ||
        ((visible.x1 != win->x1 || visible.y1 != win->y1) && !ms->drmmode.overlay_src_offsets) ||
        ms_overlay_box_on_other_crtc(crtc, win))
        return FALSE;

#ifdef HAVE_GBM
    if (ms_compositor_cursor_in_box(crtc->scrn, win))
        return FALSE;
#endif

    position->src_x = visible.x1 - win->x1;
    position->src_y = visible.y1 - win->y1;
    position->box.x1 = visible.x1 - bounds->x1;
    position->box.y1 = visible.y1 - bounds->y1;
    position->box.x2 = visible.x2 - bounds->x1;
    position->box.y2 = visible.y2 - bounds->y1;
    return TRUE;
}

Bool
ms_overlay_owner_settled(xf86CrtcPtr crtc, drmmode_overlay_owner owner)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_tearfree_frames_rec *frames = &drmmode_crtc->tearfree.frames;
    int i;

    if (drmmode_crtc->overlay_plane.shown_owner == owner)
        return FALSE;

    for (i = 0; i < frames->count; i++) {
        if (frames->frame[frames->order[i]].overlay_owner == owner)
            return FALSE;
    }
    return TRUE;
}

void
ms_overlay_update(ScreenPtr screen)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(xf86ScreenToScrn(screen));
    int i;

    for (i = 0; i < config->num_crtc; i++) {
        ms_overlay_moving_update(config->crtc[i]);
        ms_overlay_presented_update(config->crtc[i]);
    }
}

Bool
ms_overlay_active(xf86CrtcPtr crtc)
{
    return ms_overlay_moving_active(crtc) || ms_overlay_presented_active(crtc);
}

drmmode_overlay_owner
ms_overlay_frame_owner(xf86CrtcPtr crtc)
{
    if (ms_overlay_moving_active(crtc))
        return DRMMODE_OVERLAY_MOVING;
    if (ms_overlay_presented_active(crtc))
        return DRMMODE_OVERLAY_PRESENTED;
    return DRMMODE_OVERLAY_NONE;
}

Bool
ms_overlay_owner_ready(xf86CrtcPtr crtc, drmmode_overlay_owner owner)
{
    switch (owner) {
    case DRMMODE_OVERLAY_MOVING:
        return ms_overlay_moving_ready(crtc);
    case DRMMODE_OVERLAY_PRESENTED:
        return ms_overlay_presented_ready(crtc);
    default:
        return TRUE;
    }
}

void
ms_overlay_begin_commit(xf86CrtcPtr crtc, drmmode_overlay_owner owner)
{
    ms_overlay_plane(crtc)->queued_owner = owner;
}

void
ms_overlay_prepare_commit(xf86CrtcPtr crtc)
{
    switch (ms_overlay_plane(crtc)->queued_owner) {
    case DRMMODE_OVERLAY_MOVING:
        ms_overlay_moving_fill(crtc);
        break;
    case DRMMODE_OVERLAY_PRESENTED:
        ms_overlay_presented_fill(crtc);
        break;
    default:
        break;
    }
}

void
ms_overlay_end_commit(xf86CrtcPtr crtc, Bool committed, uint32_t seq)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    if (committed)
        ms_overlay_presented_committed(crtc, seq);
    overlay->queued_owner = DRMMODE_OVERLAY_NONE;
}

void
ms_overlay_shown(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    overlay->shown_owner = overlay->fb_id ? overlay->queued_owner : DRMMODE_OVERLAY_NONE;
}

Bool
ms_overlay_pending(xf86CrtcPtr crtc, uint32_t *fb_id,
                   drmmode_overlay_position_rec *position)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    if (!overlay->fb_id || ms_overlay_commit_in_flight(crtc))
        return FALSE;

    switch (overlay->shown_owner) {
    case DRMMODE_OVERLAY_MOVING:
        return ms_overlay_moving_pending(crtc, fb_id, position);
    case DRMMODE_OVERLAY_PRESENTED:
        return ms_overlay_presented_pending(crtc, fb_id, position);
    default:
        return FALSE;
    }
}

void
ms_overlay_committed_alone(xf86CrtcPtr crtc, uint32_t fb_id,
                           const drmmode_overlay_position_rec *position,
                           uint32_t seq)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    overlay->fb_id = fb_id;
    overlay->position = *position;
    ms_overlay_presented_committed(crtc, seq);
}

void
ms_overlay_commit_rejected(xf86CrtcPtr crtc, int error)
{
    if (ms_overlay_plane(crtc)->queued_owner == DRMMODE_OVERLAY_PRESENTED ||
        ms_overlay_plane(crtc)->shown_owner == DRMMODE_OVERLAY_PRESENTED)
        ms_overlay_presented_rejected(crtc);
    else
        ms_overlay_rejected(crtc, error);
}

void
ms_overlay_event_done(xf86CrtcPtr crtc, uint32_t seq, uint64_t msc,
                      uint64_t usec)
{
    ms_overlay_presented_flip_done(crtc, seq, msc, usec);
}

void
ms_overlay_event_aborted(xf86CrtcPtr crtc, uint32_t seq)
{
    uint64_t usec = 0, msc = 0;

    ms_get_crtc_ust_msc(crtc, &usec, &msc);
    ms_overlay_presented_flip_done(crtc, seq, msc, usec);
}

void
ms_overlay_box(RegionPtr cover, const BoxRec *bounds, BoxPtr box)
{
    *box = *RegionExtents(cover);
    ms_overlay_grow(&box->x1, &box->x2, MS_OVERLAY_MIN_WIDTH, bounds->x1,
                    bounds->x2);
    ms_overlay_grow(&box->y1, &box->y2, MS_OVERLAY_MIN_HEIGHT, bounds->y1,
                    bounds->y2);
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
ms_overlay_clear_next(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    if (overlay->next_fence_fd >= 0)
        close(overlay->next_fence_fd);
    overlay->next_fence_fd = -1;
    overlay->next_fb_id = 0;
}

static void
ms_overlay_forget_cover(xf86CrtcPtr crtc)
{
    ms_overlay_plane(crtc)->cover.window = NULL;
}

Bool
ms_overlay_cover_shown(xf86CrtcPtr crtc, WindowPtr window, RegionPtr cover,
                       Bool pq)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);
    drmmode_overlay_cover_rec *shown = &overlay->cover;

    return overlay->fb_id && shown->window == window && shown->pq == pq &&
        shown->serial == CompositeServerCoverSerial(window, cover) &&
        RegionEqual(&shown->region, cover);
}

void
ms_overlay_keep(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    ms_overlay_clear_next(crtc);
    overlay->next_fb_id = overlay->fb_id;
    overlay->next_position = overlay->position;
}

void
ms_overlay_queue_cover(xf86CrtcPtr crtc, const BoxRec *box, WindowPtr window,
                       RegionPtr cover, Bool pq)
{
    drmmode_overlay_cover_rec *queued = &ms_overlay_plane(crtc)->cover;

    ms_overlay_queue(crtc, box);
    queued->window = window;
    queued->pq = pq;
    queued->serial = CompositeServerCoverSerial(window, cover);
    RegionCopy(&queued->region, cover);
}

void
ms_overlay_queue(xf86CrtcPtr crtc, const BoxRec *box)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);
    drmmode_overlay_buffers_rec *buffers = &overlay->buffers;

    ms_overlay_clear_next(crtc);
    if (drmmode_crtc_can_fence_flip(crtc))
        overlay->next_fence_fd = ms->glamor.egl_create_fence_fd(screen);
    if (overlay->next_fence_fd < 0)
        ms->glamor.finish(screen);

    overlay->next_fb_id = buffers->buf[buffers->back_idx].fb_id;
    overlay->next_position = (drmmode_overlay_position_rec) { .box = *box };
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
    ms_overlay_forget_cover(crtc);
}

void
ms_overlay_flip_failed(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    overlay->buffers.pending = FALSE;
    ms_overlay_clear_next(crtc);
    ms_overlay_forget_cover(crtc);
}

void
ms_overlay_rejected(xf86CrtcPtr crtc, int error)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
               "Overlay plane %u was rejected, not using it: %s\n",
               overlay->plane_id, strerror(-error));
    ms_overlay_destroy(crtc);
    overlay->plane_id = 0;
}

void
ms_overlay_destroy(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_plane(crtc);

    ms_overlay_presented_destroy(crtc);
    ms_overlay_destroy_buffers(crtc);
    overlay->queued_owner = DRMMODE_OVERLAY_NONE;
    overlay->shown_owner = DRMMODE_OVERLAY_NONE;
    overlay->fb_id = 0;
    ms_overlay_clear_next(crtc);
    ms_overlay_forget_cover(crtc);
}
