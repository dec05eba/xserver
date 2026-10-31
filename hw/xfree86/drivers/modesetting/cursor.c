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

#include <errno.h>
#include <string.h>
#include <xf86.h>
#include <xf86Crtc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <cursorstr.h>

#include "driver.h"
#include "dumb_bo.h"

#if X_BYTE_ORDER == X_BIG_ENDIAN
#define cpu_to_le32(x) bswap_32(x)
#else
#define cpu_to_le32(x) (x)
#endif

#define MS_CURSOR_NONE -1

static drmmode_cursor_rec *
ms_cursor(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return &drmmode_crtc->cursor;
}

static int
ms_cursor_fd(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return drmmode_crtc->drmmode->fd;
}

static uint32_t
ms_cursor_crtc_id(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return drmmode_crtc->mode_crtc->crtc_id;
}

static int
ms_cursor_buffer_count(drmmode_cursor_rec *cursor)
{
    return cursor->plane.id ? ARRAY_SIZE(cursor->buf) : 1;
}

Bool
ms_cursor_create_buffers(xf86CrtcPtr crtc)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_cursor_rec *cursor = ms_cursor(crtc);
    int i;

    cursor->image = MS_CURSOR_NONE;
    cursor->plane.shown = MS_CURSOR_NONE;
    for (i = 0; i < ms_cursor_buffer_count(cursor); i++) {
        cursor->buf[i].bo = dumb_bo_create(ms_cursor_fd(crtc),
                                           ms->max_cursor_width,
                                           ms->max_cursor_height, 32);
        if (!cursor->buf[i].bo)
            return FALSE;
    }
    return TRUE;
}

Bool
ms_cursor_map_buffers(ScrnInfoPtr scrn)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
    int c, i;

    for (c = 0; c < xf86_config->num_crtc; c++) {
        xf86CrtcPtr crtc = xf86_config->crtc[c];
        drmmode_cursor_rec *cursor = ms_cursor(crtc);

        for (i = 0; i < ms_cursor_buffer_count(cursor); i++) {
            if (dumb_bo_map(ms_cursor_fd(crtc), cursor->buf[i].bo))
                return FALSE;
        }
    }
    return TRUE;
}

void
ms_cursor_destroy_buffers(xf86CrtcPtr crtc)
{
    drmmode_cursor_rec *cursor = ms_cursor(crtc);
    int fd = ms_cursor_fd(crtc);
    int i;

    for (i = 0; i < ARRAY_SIZE(cursor->buf); i++) {
        drmmode_cursor_buffer_rec *buf = &cursor->buf[i];

        if (buf->fb_id)
            drmModeRmFB(fd, buf->fb_id);
        if (buf->bo)
            dumb_bo_destroy(fd, buf->bo);
        *buf = (drmmode_cursor_buffer_rec) { 0 };
    }
}

void
ms_cursor_probe_sizes(xf86CrtcPtr crtc)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    uint32_t handle = ms_cursor(crtc)->buf[0].bo->handle;
    uint32_t crtc_id = ms_cursor_crtc_id(crtc);
    int fd = ms_cursor_fd(crtc);
    int width, height, size;

    ms->min_cursor_width = ms->max_cursor_width;
    ms->min_cursor_height = ms->max_cursor_height;

    for (size = 1; size <= ms->max_cursor_width &&
         size <= ms->max_cursor_height; size *= 2) {
        if (drmModeSetCursor2(fd, crtc_id, handle, size, size, 0, 0) == 0)
            break;
    }

    for (width = 1; width <= size; width *= 2) {
        if (drmModeSetCursor2(fd, crtc_id, handle, width, size, 0, 0) == 0) {
            ms->min_cursor_width = width;
            break;
        }
    }

    for (height = 1; height <= size; height *= 2) {
        if (drmModeSetCursor2(fd, crtc_id, handle, size, height, 0, 0) == 0) {
            ms->min_cursor_height = height;
            break;
        }
    }

    drmModeSetCursor2(fd, crtc_id, 0, 0, 0, 0, 0);
}

static int
ms_cursor_wanted(drmmode_cursor_rec *cursor)
{
    return cursor->up ? cursor->image : MS_CURSOR_NONE;
}

Bool
ms_cursor_in_flight(xf86CrtcPtr crtc)
{
    return ms_cursor(crtc)->plane.flight_seq != 0;
}

Bool
ms_cursor_needs_commit(xf86CrtcPtr crtc)
{
    drmmode_cursor_rec *cursor = ms_cursor(crtc);

    return cursor->plane.id && !cursor->plane.flight_seq &&
        ms_cursor_wanted(cursor) != cursor->plane.shown;
}

static Bool
ms_cursor_set_legacy(xf86CrtcPtr crtc)
{
    drmmode_cursor_rec *cursor = ms_cursor(crtc);
    CursorPtr current = xf86CurrentCursor(crtc->scrn->pScreen);
    uint32_t handle = cursor->buf[cursor->image].bo->handle;
    uint32_t crtc_id = ms_cursor_crtc_id(crtc);
    int fd = ms_cursor_fd(crtc);
    int ret;

    if (current == NullCursor)
        return TRUE;

    ret = drmModeSetCursor2(fd, crtc_id, handle, cursor->width, cursor->height,
                            current->bits->xhot, current->bits->yhot);

    /* -EINVAL can mean that an old kernel supports drmModeSetCursor but
     * not drmModeSetCursor2, though it can mean other things too. */
    if (ret == -EINVAL)
        ret = drmModeSetCursor(fd, crtc_id, handle, cursor->width,
                               cursor->height);

    /* -ENXIO normally means that the current drm driver supports neither
     * cursor_set nor cursor_set2.  Disable hardware cursor support for
     * the rest of the session in that case. */
    if (ret == -ENXIO) {
        xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
        xf86CursorInfoPtr cursor_info = xf86_config->cursor_info;

        cursor_info->MaxWidth = cursor_info->MaxHeight = 0;
        modesettingPTR(crtc->scrn)->drmmode.sw_cursor = TRUE;
    }

    return ret == 0;
}

void
ms_cursor_rejected(xf86CrtcPtr crtc, int error)
{
    drmmode_cursor_rec *cursor = ms_cursor(crtc);

    xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
               "Cursor plane %u was rejected, using the legacy cursor: %s\n",
               cursor->plane.id, strerror(-error));
    cursor->plane.id = 0;
    cursor->plane.flight_seq = 0;
    cursor->plane.shown = MS_CURSOR_NONE;
    if (cursor->up && cursor->image != MS_CURSOR_NONE)
        ms_cursor_set_legacy(crtc);
}

static Bool
ms_cursor_buffer_busy(drmmode_cursor_plane_rec *plane, int index)
{
    return index == plane->shown ||
        (plane->flight_seq && index == plane->flight);
}

static drmmode_cursor_buffer_rec *
ms_cursor_next_buffer(drmmode_cursor_rec *cursor)
{
    int i;

    if (!cursor->plane.id)
        return &cursor->buf[0];

    if (cursor->image != MS_CURSOR_NONE &&
        !ms_cursor_buffer_busy(&cursor->plane, cursor->image))
        return &cursor->buf[cursor->image];

    for (i = 0; i < ARRAY_SIZE(cursor->buf); i++) {
        if (!ms_cursor_buffer_busy(&cursor->plane, i))
            return &cursor->buf[i];
    }
    return &cursor->buf[0];
}

static int
ms_cursor_buffer_fb(xf86CrtcPtr crtc, drmmode_cursor_buffer_rec *buf,
                    int width, int height)
{
    int fd = ms_cursor_fd(crtc);
    uint32_t handles[4] = { buf->bo->handle };
    uint32_t pitches[4] = { width * 4 };
    uint32_t offsets[4] = { 0 };

    if (buf->fb_id && buf->width == width && buf->height == height)
        return 0;

    if (buf->fb_id)
        drmModeRmFB(fd, buf->fb_id);
    buf->fb_id = 0;
    buf->width = width;
    buf->height = height;
    return drmModeAddFB2(fd, width, height, DRM_FORMAT_ARGB8888, handles,
                         pitches, offsets, &buf->fb_id, 0);
}

static uint32_t
ms_cursor_pixel_to_pq(uint32_t pixel, const uint8_t *pq)
{
    uint32_t alpha = pixel >> 24;
    uint32_t pq_pixel = pixel & 0xff000000;
    int shift;

    if (!alpha)
        return pixel;

    for (shift = 0; shift < 24; shift += 8) {
        uint32_t straight = min(((pixel >> shift) & 0xff) * 255 / alpha, 255);

        pq_pixel |= (pq[straight] * alpha / 255) << shift;
    }
    return pq_pixel;
}

/* Only enabled for some kms drivers, as some display hw is known broken with
 * cursor sizes other than the maximum. Only square sizes are assumed to work.
 */
static void
ms_cursor_image_size(xf86CrtcPtr crtc, CursorPtr current, int *width,
                     int *height)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);

    if (crtc->rotation != RR_Rotate_0 || !ms->allow_cursor_size_optim) {
        *width = ms->max_cursor_width;
        *height = ms->max_cursor_height;
        return;
    }

    for (*width = ms->min_cursor_width; *width < current->bits->width; )
        *width *= 2;
    for (*height = ms->min_cursor_height; *height < current->bits->height; )
        *height *= 2;

    *width = *height = max(*width, *height);
    *width = min(*width, ms->max_cursor_width);
    *height = min(*height, ms->max_cursor_height);
}

Bool
ms_cursor_load_argb(xf86CrtcPtr crtc, CARD32 *image)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    const drmmode_hdr_state_rec *hdr = &drmmode_crtc->hdr.current;
    drmmode_cursor_rec *cursor = &drmmode_crtc->cursor;
    drmmode_cursor_buffer_rec *buf = ms_cursor_next_buffer(cursor);
    CursorPtr current = xf86CurrentCursor(crtc->scrn->pScreen);
    Bool pq = drmmode_hdr_cursor_expects_pq(hdr);
    uint8_t pq_table[256];
    uint32_t *ptr = buf->bo->ptr;
    int width, height, x, y, i, ret;

    if (pq)
        ms_hdr_sdr_to_pq_table(hdr->sdr_white_level, pq_table);

    ms_cursor_image_size(crtc, current, &width, &height);
    cursor->width = width;
    cursor->height = height;

    i = 0;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint32_t pixel = image[y * ms->max_cursor_width + x];

            ptr[i++] = cpu_to_le32(pq ? ms_cursor_pixel_to_pq(pixel, pq_table) :
                                   pixel);
        }
    }
    while (i < ms->max_cursor_width * ms->max_cursor_height)
        ptr[i++] = 0;

    cursor->image = buf - cursor->buf;
    if (cursor->plane.id) {
        ret = ms_cursor_buffer_fb(crtc, buf, width, height);
        if (ret)
            ms_cursor_rejected(crtc, ret);
        return TRUE;
    }
    return !cursor->up || ms_cursor_set_legacy(crtc);
}

Bool
ms_cursor_show(xf86CrtcPtr crtc)
{
    drmmode_cursor_rec *cursor = ms_cursor(crtc);

    cursor->up = TRUE;
    if (cursor->plane.id || cursor->image == MS_CURSOR_NONE)
        return TRUE;
    return ms_cursor_set_legacy(crtc);
}

void
ms_cursor_hide(xf86CrtcPtr crtc)
{
    drmmode_cursor_rec *cursor = ms_cursor(crtc);

    cursor->up = FALSE;
    if (!cursor->plane.id)
        drmModeSetCursor(ms_cursor_fd(crtc), ms_cursor_crtc_id(crtc), 0,
                         cursor->width, cursor->height);
}

void
ms_cursor_set_position(xf86CrtcPtr crtc, int x, int y)
{
    drmmode_cursor_rec *cursor = ms_cursor(crtc);

    cursor->x = x;
    cursor->y = y;
    drmModeMoveCursor(ms_cursor_fd(crtc), ms_cursor_crtc_id(crtc), x, y);
}

void
ms_cursor_set_colors(xf86CrtcPtr crtc, int bg, int fg)
{
}

int
ms_cursor_add_props(drmModeAtomicReq *req, xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_cursor_rec *cursor = &drmmode_crtc->cursor;
    uint32_t plane_id = cursor->plane.id;
    int wanted = ms_cursor_wanted(cursor);
    drmmode_cursor_buffer_rec *buf =
        wanted == MS_CURSOR_NONE ? NULL : &cursor->buf[wanted];
    uint32_t fb_id = buf ? buf->fb_id : 0;
    int ret = 0;

    ret |= drmmode_plane_add_prop(req, drmmode_crtc, plane_id,
                                  DRMMODE_PLANE_FB_ID, fb_id);
    ret |= drmmode_plane_add_prop(req, drmmode_crtc, plane_id,
                                  DRMMODE_PLANE_CRTC_ID,
                                  fb_id ? drmmode_crtc->mode_crtc->crtc_id : 0);
    if (!fb_id)
        return ret;

    ret |= drmmode_plane_add_prop(req, drmmode_crtc, plane_id,
                                  DRMMODE_PLANE_SRC_X, 0);
    ret |= drmmode_plane_add_prop(req, drmmode_crtc, plane_id,
                                  DRMMODE_PLANE_SRC_Y, 0);
    ret |= drmmode_plane_add_prop(req, drmmode_crtc, plane_id,
                                  DRMMODE_PLANE_SRC_W, buf->width << 16);
    ret |= drmmode_plane_add_prop(req, drmmode_crtc, plane_id,
                                  DRMMODE_PLANE_SRC_H, buf->height << 16);
    ret |= drmmode_plane_add_prop(req, drmmode_crtc, plane_id,
                                  DRMMODE_PLANE_CRTC_X, (int64_t) cursor->x);
    ret |= drmmode_plane_add_prop(req, drmmode_crtc, plane_id,
                                  DRMMODE_PLANE_CRTC_Y, (int64_t) cursor->y);
    ret |= drmmode_plane_add_prop(req, drmmode_crtc, plane_id,
                                  DRMMODE_PLANE_CRTC_W, buf->width);
    ret |= drmmode_plane_add_prop(req, drmmode_crtc, plane_id,
                                  DRMMODE_PLANE_CRTC_H, buf->height);
    return ret;
}

void
ms_cursor_committed(xf86CrtcPtr crtc, uint32_t seq, Bool alone)
{
    drmmode_cursor_rec *cursor = ms_cursor(crtc);

    cursor->plane.flight = ms_cursor_wanted(cursor);
    cursor->plane.flight_seq = seq;
    cursor->plane.flight_alone = alone;
}

/* A commit that only changes the cursor may complete before the next vblank */
Bool
ms_cursor_only_event(xf86CrtcPtr crtc, uint32_t seq)
{
    drmmode_cursor_plane_rec *plane = &ms_cursor(crtc)->plane;

    return seq && plane->flight_seq == seq && plane->flight_alone;
}

void
ms_cursor_flip_done(xf86CrtcPtr crtc, uint32_t seq)
{
    drmmode_cursor_plane_rec *plane = &ms_cursor(crtc)->plane;

    if (!seq || plane->flight_seq != seq)
        return;

    plane->shown = plane->flight;
    plane->flight_seq = 0;
#ifdef HAVE_GBM
    if (ms_prime_crtc_flipping(crtc))
        ms_prime_kick(crtc);
#endif
}

static void
ms_cursor_handler(uint64_t msc, uint64_t usec, void *data)
{
}

static void
ms_cursor_abort(void *data)
{
}

static int
ms_cursor_do_commit(xf86CrtcPtr crtc, uint32_t flags, uint32_t seq)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    int ret;

    if (!req)
        return -ENOMEM;

    ret = ms_cursor_add_props(req, crtc);
    if (ret == 0)
        ret = drmModeAtomicCommit(ms_cursor_fd(crtc), req, flags,
                                  (void *) (uintptr_t) seq);
    drmModeAtomicFree(req);
    return ret;
}

Bool
ms_cursor_test(xf86CrtcPtr crtc)
{
    return ms_cursor_do_commit(crtc, DRM_MODE_ATOMIC_TEST_ONLY, 0) == 0;
}

static void
ms_cursor_commit(xf86CrtcPtr crtc)
{
    ScrnInfoPtr scrn = crtc->scrn;
    uint32_t seq;
    int ret;

    if (!scrn->vtSema || !xf86_crtc_on(crtc))
        return;

    seq = ms_drm_queue_alloc(crtc, crtc, ms_cursor_handler, ms_cursor_abort);
    if (!seq)
        return;

    ret = ms_cursor_do_commit(crtc, DRM_MODE_ATOMIC_NONBLOCK |
                              DRM_MODE_PAGE_FLIP_EVENT, seq);
    if (ret == 0) {
        ms_cursor_committed(crtc, seq, TRUE);
        return;
    }

    ms_drm_abort_seq(scrn, seq);
    if (ret != -EBUSY)
        ms_cursor_rejected(crtc, ret);
}

static Bool
ms_cursor_flip_expected(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    if (drmmode_crtc->tearfree.flip_seq)
        return TRUE;
#ifdef HAVE_GBM
    if (ms_prime_crtc_flipping(crtc))
        return ms_prime_flip_wanted(crtc);
    return ms_tearfree_is_active_on_crtc(crtc) && ms_tearfree_flip_wanted(crtc);
#else
    return FALSE;
#endif
}

void
ms_cursor_commit_pending(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
    int c;

    for (c = 0; c < xf86_config->num_crtc; c++) {
        xf86CrtcPtr crtc = xf86_config->crtc[c];

        if (ms_cursor_needs_commit(crtc) && !ms_cursor_flip_expected(crtc))
            ms_cursor_commit(crtc);
    }
}
