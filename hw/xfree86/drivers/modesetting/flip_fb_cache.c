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

#include "driver.h"

static drmmode_scanout_rec *
ms_flip_fb_cache_scanout(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return &drmmode_crtc->tearfree.scanout;
}

static Bool
ms_flip_fb_cache_in_use(drmmode_scanout_rec *scanout, uint32_t fb_id)
{
    return fb_id == scanout->fb_id || fb_id == scanout->flip_fb_id;
}

static void
ms_flip_fb_cache_drop(xf86CrtcPtr crtc, drmmode_flip_fb_rec *flip_fb)
{
    drmmode_close_fb(modesettingPTR(crtc->scrn)->fd, flip_fb->fb_id);
    dixDestroyPixmap(flip_fb->pixmap, 0);
    *flip_fb = (drmmode_flip_fb_rec) { 0 };
}

static drmmode_flip_fb_rec *
ms_flip_fb_cache_find(drmmode_flip_fb_cache_rec *cache, PixmapPtr pixmap, uint64_t image_id)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(cache->fbs); i++) {
        if (cache->fbs[i].pixmap == pixmap && cache->fbs[i].image_id == image_id)
            return &cache->fbs[i];
    }
    return NULL;
}

static drmmode_flip_fb_rec *
ms_flip_fb_cache_free_slot(drmmode_scanout_rec *scanout)
{
    drmmode_flip_fb_cache_rec *cache = &scanout->fb_cache;
    int i;

    for (i = 0; i < ARRAY_SIZE(cache->fbs); i++) {
        int idx = (cache->next + i) % ARRAY_SIZE(cache->fbs);
        drmmode_flip_fb_rec *flip_fb = &cache->fbs[idx];

        if (!flip_fb->pixmap || !ms_flip_fb_cache_in_use(scanout, flip_fb->fb_id)) {
            cache->next = (idx + 1) % ARRAY_SIZE(cache->fbs);
            return flip_fb;
        }
    }
    return NULL;
}

/* Adding and removing framebuffers can block for many ms in nvidia-drm while
 * the GPU is busy, so directly scanned out pixmaps keep their framebuffers
 * for their next flips.
 */
uint32_t
ms_flip_fb_cache_get(xf86CrtcPtr crtc, PixmapPtr pixmap)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_scanout_rec *scanout = ms_flip_fb_cache_scanout(crtc);
    uint64_t image_id = ms->glamor.egl_pixmap_image_id ? ms->glamor.egl_pixmap_image_id(pixmap) : 0;
    drmmode_flip_fb_rec *flip_fb;
    uint32_t fb_id;

    if (image_id && (flip_fb = ms_flip_fb_cache_find(&scanout->fb_cache, pixmap, image_id)))
        return flip_fb->fb_id;

    if (!ms_import_flip_fb(xf86ScrnToScreen(crtc->scrn), pixmap, &fb_id, "Direct-scanout"))
        return 0;

    if (!image_id || !(flip_fb = ms_flip_fb_cache_free_slot(scanout)))
        return fb_id;

    if (flip_fb->pixmap)
        ms_flip_fb_cache_drop(crtc, flip_fb);
    pixmap->refcnt++;
    *flip_fb = (drmmode_flip_fb_rec) {
        .pixmap = pixmap,
        .image_id = image_id,
        .fb_id = fb_id,
    };
    return fb_id;
}

void
ms_flip_fb_cache_release(xf86CrtcPtr crtc, uint32_t fb_id)
{
    drmmode_flip_fb_cache_rec *cache = &ms_flip_fb_cache_scanout(crtc)->fb_cache;
    int i;

    if (!fb_id)
        return;

    for (i = 0; i < ARRAY_SIZE(cache->fbs); i++) {
        if (cache->fbs[i].pixmap && cache->fbs[i].fb_id == fb_id)
            return;
    }
    drmmode_close_fb(modesettingPTR(crtc->scrn)->fd, fb_id);
}

void
ms_flip_fb_cache_drop_unused(xf86CrtcPtr crtc)
{
    drmmode_scanout_rec *scanout = ms_flip_fb_cache_scanout(crtc);
    int i;

    for (i = 0; i < ARRAY_SIZE(scanout->fb_cache.fbs); i++) {
        drmmode_flip_fb_rec *flip_fb = &scanout->fb_cache.fbs[i];

        if (flip_fb->pixmap && !ms_flip_fb_cache_in_use(scanout, flip_fb->fb_id))
            ms_flip_fb_cache_drop(crtc, flip_fb);
    }
}
