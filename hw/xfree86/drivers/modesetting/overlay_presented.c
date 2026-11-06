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

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <compositeext.h>

#include "driver.h"
#include "present.h"

#define MS_OVERLAY_PRESENTED_MIN_FLIPS 8
#define MS_OVERLAY_PRESENTED_STREAK_USEC 200000
#define MS_OVERLAY_PRESENTED_GAP_USEC 200000
#define MS_OVERLAY_PRESENTED_REALLOC_USEC 2000000
#define MS_OVERLAY_PRESENTED_RECENT_USEC 5000000

static drmmode_overlay_plane_rec *
ms_overlay_presented_plane(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return &drmmode_crtc->overlay_plane;
}

static drmmode_overlay_presented_rec *
ms_overlay_presented(xf86CrtcPtr crtc)
{
    return &ms_overlay_presented_plane(crtc)->presented;
}

static void
ms_overlay_presented_rmfb(xf86CrtcPtr crtc, uint32_t fb_id)
{
    if (fb_id)
        drmModeRmFB(modesettingPTR(crtc->scrn)->fd, fb_id);
}

static void
ms_overlay_presented_drop_pending(xf86CrtcPtr crtc)
{
    drmmode_overlay_fbs_rec *fbs = &ms_overlay_presented(crtc)->fbs;

    if (fbs->pending != fbs->shown && fbs->pending != fbs->retired)
        ms_overlay_presented_rmfb(crtc, fbs->pending);
    fbs->pending = 0;
    fbs->pixmap = NULL;
    fbs->pixmap_id = 0;
}

static Bool
ms_overlay_presented_import(xf86CrtcPtr crtc, PixmapPtr pixmap)
{
    drmmode_overlay_fbs_rec *fbs = &ms_overlay_presented(crtc)->fbs;
    uint32_t fb_id;

    if (fbs->pending && fbs->pixmap == pixmap && fbs->pixmap_id == pixmap->drawable.id)
        return TRUE;

    if (!ms_import_flip_fb(xf86ScrnToScreen(crtc->scrn), pixmap, &fb_id, "Overlay plane"))
        return FALSE;

    ms_overlay_presented_drop_pending(crtc);
    fbs->pending = fb_id;
    fbs->pixmap = pixmap;
    fbs->pixmap_id = pixmap->drawable.id;
    return TRUE;
}

static void
ms_overlay_presented_release(xf86CrtcPtr crtc)
{
    drmmode_overlay_presented_rec *presented = ms_overlay_presented(crtc);

    if (presented->window || presented->fbs.flight_seq ||
        !ms_overlay_owner_settled(crtc, DRMMODE_OVERLAY_PRESENTED))
        return;

    ms_overlay_presented_drop_pending(crtc);
}

static Bool
ms_overlay_presented_rate_ok(const drmmode_overlay_flip_rate_rec *rate)
{
    uint64_t now = GetTimeInMicros();

    return rate->flips >= MS_OVERLAY_PRESENTED_MIN_FLIPS &&
        now - rate->first_flip >= MS_OVERLAY_PRESENTED_STREAK_USEC &&
        now - rate->last_flip <= MS_OVERLAY_PRESENTED_GAP_USEC;
}

static Bool
ms_overlay_presented_crtc_usable(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_presented_plane(crtc);
    Bool pq;

    return !ms_overlay_moving_active(crtc) && ms_overlay_crtc_usable(crtc, &pq) &&
        (!pq || overlay->color.pipeline_prop) &&
        !CompositeServerScanoutWindow(xf86ScrnToScreen(crtc->scrn), &crtc->bounds);
}

static WindowPtr
ms_overlay_presented_candidate(xf86CrtcPtr crtc, PixmapPtr *pixmap,
                               drmmode_overlay_position_rec *position)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    drmmode_overlay_presented_rec *presented = ms_overlay_presented(crtc);
    XID window_id = presented->window ? presented->window->drawable.id : presented->rate.window_id;
    WindowPtr window, plane_window;
    PixmapPtr flip;

    if (!window_id || !ms_overlay_presented_crtc_usable(crtc) ||
        dixLookupWindow(&window, window_id, serverClient, DixGetAttrAccess) != Success ||
        window->drawable.pScreen != screen || window->drawable.id == presented->rejected_id ||
        (window != presented->window && !ms_overlay_presented_rate_ok(&presented->rate)) ||
        !CompositeServerWindowUnobscured(window))
        return NULL;

    plane_window = CompositeServerPresentPlaneWindow(screen);
    if (plane_window && plane_window != presented->window)
        return NULL;

    flip = present_window_flip_pixmap(window);
    if (!flip || flip->drawable.width != window->drawable.width ||
        flip->drawable.height != window->drawable.height ||
        !ms_pixmap_scanout_format_supported(screen, flip) ||
        !ms_overlay_window_position(crtc, RegionExtents(&window->borderSize), position))
        return NULL;

    *pixmap = flip;
    return window;
}

static void
ms_overlay_presented_demote(xf86CrtcPtr crtc)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    drmmode_overlay_presented_rec *presented = ms_overlay_presented(crtc);

    if (CompositeServerPresentPlaneWindow(screen) == presented->window)
        CompositeServerSetPresentPlaneWindow(screen, NULL);
    if (presented->window) {
        presented->recent.window_id = presented->window->drawable.id;
        presented->recent.until = GetTimeInMicros() + MS_OVERLAY_PRESENTED_RECENT_USEC;
    }
    presented->window = NULL;
    ms_tearfree_plane_flips_requeue(crtc);
}

static void
ms_overlay_presented_promote(xf86CrtcPtr crtc, WindowPtr window, PixmapPtr pixmap,
                             const drmmode_overlay_position_rec *position)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_presented_plane(crtc);
    drmmode_overlay_presented_rec *presented = &overlay->presented;
    Bool accepted;

    if (!ms_overlay_owner_settled(crtc, DRMMODE_OVERLAY_PRESENTED) ||
        !ms_overlay_presented_import(crtc, pixmap))
        return;

    overlay->next_fb_id = presented->fbs.pending;
    overlay->next_position = *position;
    overlay->next_convert_sdr = TRUE;
    accepted = drmmode_overlay_test(crtc);
    ms_overlay_clear_next(crtc);

    if (!accepted) {
        xf86DrvMsg(crtc->scrn->scrnIndex, X_INFO,
                   "Overlay plane rejected the buffers of window 0x%x, compositing it\n",
                   (unsigned int) window->drawable.id);
        presented->rejected_id = window->drawable.id;
        ms_overlay_presented_drop_pending(crtc);
        return;
    }

    presented->window = window;
    presented->position = *position;
    CompositeServerSetPresentPlaneWindow(xf86ScrnToScreen(crtc->scrn), window);
}

void
ms_overlay_presented_update(xf86CrtcPtr crtc)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    drmmode_overlay_presented_rec *presented = ms_overlay_presented(crtc);
    drmmode_overlay_position_rec position;
    PixmapPtr pixmap = NULL;
    WindowPtr window;

    if (presented->window && CompositeServerPresentPlaneWindow(screen) != presented->window) {
        presented->window = NULL;
        ms_tearfree_plane_flips_requeue(crtc);
    }

    window = ms_overlay_presented_candidate(crtc, &pixmap, &position);
    if (presented->window && window != presented->window)
        ms_overlay_presented_demote(crtc);

    if (!window) {
        ms_overlay_presented_release(crtc);
        return;
    }

    if (!presented->window) {
        ms_overlay_presented_promote(crtc, window, pixmap, &position);
        return;
    }

    presented->position = position;
    if (!ms_overlay_presented_import(crtc, pixmap)) {
        presented->rejected_id = window->drawable.id;
        ms_overlay_presented_demote(crtc);
    }
}

static xf86CrtcPtr
ms_overlay_presented_crtc_for(WindowPtr window)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(xf86ScreenToScrn(window->drawable.pScreen));
    int i;

    for (i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];
        drmmode_overlay_presented_rec *presented = ms_overlay_presented(crtc);

        if ((presented->window == window ||
             (presented->rate.window_id == window->drawable.id &&
              ms_overlay_presented_rate_ok(&presented->rate))) &&
            window->drawable.id != presented->rejected_id &&
            ms_overlay_presented_crtc_usable(crtc))
            return crtc;
    }
    return NULL;
}

static Bool
ms_overlay_presented_recently_shown(WindowPtr window)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(xf86ScreenToScrn(window->drawable.pScreen));
    uint64_t now = GetTimeInMicros();
    int i;

    for (i = 0; i < config->num_crtc; i++) {
        drmmode_overlay_presented_rec *presented = ms_overlay_presented(config->crtc[i]);

        if (presented->recent.window_id == window->drawable.id &&
            now <= presented->recent.until && window->drawable.id != presented->rejected_id)
            return TRUE;
    }
    return FALSE;
}

Bool
ms_overlay_presented_wants_scanout(WindowPtr window)
{
    return ms_overlay_presented_recently_shown(window) ||
        (ms_overlay_presented_crtc_for(window) && CompositeServerWindowUnobscured(window));
}

Bool
ms_overlay_presented_suboptimal(WindowPtr window, PixmapPtr pixmap)
{
    drmmode_overlay_presented_rec *presented;
    xf86CrtcPtr crtc;
    uint64_t now;

    if (!(crtc = ms_overlay_presented_crtc_for(window)) || !CompositeServerWindowUnobscured(window) ||
        ms_pixmap_scanout_format_supported(window->drawable.pScreen, pixmap))
        return FALSE;

    presented = ms_overlay_presented(crtc);
    now = GetTimeInMicros();
    if (presented->realloc.window_id != window->drawable.id) {
        presented->realloc.window_id = window->drawable.id;
        presented->realloc.since = now;
    }
    if (now - presented->realloc.since <= MS_OVERLAY_PRESENTED_REALLOC_USEC)
        return TRUE;

    xf86DrvMsg(crtc->scrn->scrnIndex, X_INFO,
               "Window 0x%x kept buffers the overlay plane can't show, compositing it\n",
               (unsigned int) window->drawable.id);
    presented->rejected_id = window->drawable.id;
    return FALSE;
}

void
ms_overlay_presented_window_flipped(xf86CrtcPtr crtc, WindowPtr window)
{
    drmmode_overlay_flip_rate_rec *rate = &ms_overlay_presented(crtc)->rate;
    uint64_t now = GetTimeInMicros();

    if (rate->window_id != window->drawable.id ||
        now - rate->last_flip > MS_OVERLAY_PRESENTED_GAP_USEC) {
        rate->window_id = window->drawable.id;
        rate->first_flip = now;
        rate->flips = 0;
    }
    rate->flips++;
    rate->last_flip = now;
}

Bool
ms_overlay_presented_shows(xf86CrtcPtr crtc, WindowPtr window)
{
    return ms_overlay_presented(crtc)->window == window;
}

Bool
ms_overlay_presented_active(xf86CrtcPtr crtc)
{
    return ms_overlay_presented(crtc)->window != NULL;
}

Bool
ms_overlay_presented_ready(xf86CrtcPtr crtc)
{
    return ms_overlay_presented(crtc)->fbs.pending != 0;
}

void
ms_overlay_presented_fill(xf86CrtcPtr crtc)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_presented_plane(crtc);

    overlay->next_fb_id = overlay->presented.fbs.pending;
    overlay->next_position = overlay->presented.position;
    overlay->next_convert_sdr = TRUE;
}

Bool
ms_overlay_presented_pending(xf86CrtcPtr crtc, uint32_t *fb_id,
                             drmmode_overlay_position_rec *position)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_presented_plane(crtc);
    drmmode_overlay_presented_rec *presented = &overlay->presented;

    if (!presented->window || !presented->fbs.pending)
        return FALSE;

    *fb_id = presented->fbs.pending;
    *position = presented->position;
    return *fb_id != overlay->fb_id ||
        !ms_overlay_position_equal(position, &overlay->position);
}

void
ms_overlay_presented_committed(xf86CrtcPtr crtc, uint32_t seq)
{
    drmmode_overlay_plane_rec *overlay = ms_overlay_presented_plane(crtc);
    drmmode_overlay_fbs_rec *fbs = &overlay->presented.fbs;
    Bool shows = overlay->shown_owner == DRMMODE_OVERLAY_PRESENTED &&
        overlay->fb_id && overlay->fb_id == fbs->pending;

    if (!shows && (!fbs->shown || overlay->fb_id == fbs->shown))
        return;

    if (fbs->retired && fbs->retired != fbs->pending && fbs->retired != fbs->shown)
        ms_overlay_presented_rmfb(crtc, fbs->retired);
    fbs->retired = 0;

    if (shows) {
        if (fbs->shown != fbs->pending) {
            fbs->retired = fbs->shown;
            fbs->shown = fbs->pending;
        }
        ms_tearfree_plane_flips_committed(crtc);
    } else {
        fbs->retired = fbs->shown;
        fbs->shown = 0;
    }
    fbs->flight_seq = seq;
}

void
ms_overlay_presented_flip_done(xf86CrtcPtr crtc, uint32_t seq, uint64_t msc,
                               uint64_t usec)
{
    drmmode_overlay_fbs_rec *fbs = &ms_overlay_presented(crtc)->fbs;

    if (!seq || seq != fbs->flight_seq)
        return;

    fbs->flight_seq = 0;
    if (fbs->retired != fbs->pending && fbs->retired != fbs->shown)
        ms_overlay_presented_rmfb(crtc, fbs->retired);
    fbs->retired = 0;

    ms_tearfree_plane_flips_shown(crtc, msc, usec);
    ms_overlay_presented_release(crtc);
}

void
ms_overlay_presented_rejected(xf86CrtcPtr crtc)
{
    drmmode_overlay_presented_rec *presented = ms_overlay_presented(crtc);

    xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
               "Overlay plane rejected a window's buffer, compositing it\n");
    presented->rejected_id = presented->window ? presented->window->drawable.id :
        presented->rate.window_id;
    ms_overlay_presented_demote(crtc);
}

void
ms_overlay_presented_destroy(xf86CrtcPtr crtc)
{
    drmmode_overlay_fbs_rec *fbs = &ms_overlay_presented(crtc)->fbs;

    if (ms_overlay_presented_active(crtc))
        ms_overlay_presented_demote(crtc);

    if (fbs->retired != fbs->shown && fbs->retired != fbs->pending)
        ms_overlay_presented_rmfb(crtc, fbs->retired);
    if (fbs->shown != fbs->pending)
        ms_overlay_presented_rmfb(crtc, fbs->shown);
    ms_overlay_presented_rmfb(crtc, fbs->pending);
    *fbs = (drmmode_overlay_fbs_rec) { 0 };
}
