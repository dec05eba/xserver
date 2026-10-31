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

#include "driver.h"

#ifdef HAVE_GBM
#include <gbm.h>
#include <drm_fourcc.h>

static drmmode_prime_scanout_rec *
ms_prime_scanout(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return &drmmode_crtc->prime_scanout;
}

static void
ms_prime_release_import(drmmode_prime_import_rec *import)
{
    if (import->imported)
        dixDestroyPixmap(import->imported, 0);
    if (import->source)
        dixDestroyPixmap(import->source, 0);
    import->source = NULL;
    import->imported = NULL;
}

static void
ms_prime_release_fb(xf86CrtcPtr crtc, drmmode_prime_fb_rec *fb)
{
    if (fb->fb_id)
        drmModeRmFB(modesettingPTR(crtc->scrn)->fd, fb->fb_id);
    if (fb->pixmap)
        dixDestroyPixmap(fb->pixmap, 0);
    *fb = (drmmode_prime_fb_rec) { 0 };
}

static void
ms_prime_free_cover(drmmode_prime_overlay_rec *overlay)
{
    if (overlay->import)
        dixDestroyPixmap(overlay->import, 0);
    if (overlay->cover) {
        dixDestroyPixmap(overlay->cover, 0);
        close(overlay->dmabuf_fd);
    }
    if (overlay->cover_sdr)
        dixDestroyPixmap(overlay->cover_sdr, 0);
    overlay->import = NULL;
    overlay->cover = NULL;
    overlay->cover_sdr = NULL;
}

static void
ms_prime_overlay_prepare(xf86CrtcPtr crtc)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    PixmapPtr import = prime_scanout->overlay.import;
    BoxPtr box = &prime_scanout->direct.cover_box;
    PixmapPtr back;
    GCPtr gc;

    if (!import || !(back = ms_overlay_back_buffer(crtc)) ||
        !(gc = GetScratchGC(back->drawable.depth, screen)))
        return;

    ValidateGC(&back->drawable, gc);
    gc->ops->CopyArea(&import->drawable, &back->drawable, gc, 0, 0,
                      box->x2 - box->x1, box->y2 - box->y1, 0, 0);
    FreeScratchGC(gc);
    ms->glamor.finish(screen);

    ms_overlay_queue(crtc, box);
}

void
ms_prime_clear_direct(xf86CrtcPtr crtc)
{
    drmmode_prime_direct_rec *direct = &ms_prime_scanout(crtc)->direct;
    int i;

    for (i = 0; i < ARRAY_SIZE(direct->imports); i++)
        ms_prime_release_import(&direct->imports[i]);
    memset(direct, 0, sizeof(*direct));
}

void
ms_prime_destroy_scanout(xf86CrtcPtr crtc)
{
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    int i;

    for (i = 0; i < ARRAY_SIZE(prime_scanout->buf); i++) {
        if (prime_scanout->buf[i].px) {
            drmmode_shadow_fb_destroy(crtc, prime_scanout->buf[i].px,
                                      (void *)(long)1,
                                      &prime_scanout->buf[i].bo,
                                      &prime_scanout->buf[i].fb_id);
            prime_scanout->buf[i].px = NULL;
            RegionUninit(&prime_scanout->buf[i].dmg);
        }
    }
    prime_scanout->back_idx = 0;

    if (prime_scanout->composition.intermediate)
        dixDestroyPixmap(prime_scanout->composition.intermediate, 0);
    memset(&prime_scanout->composition, 0, sizeof(prime_scanout->composition));

    ms_prime_clear_direct(crtc);
    ms_prime_release_fb(crtc, &prime_scanout->direct_fbs.pending);
    ms_prime_release_fb(crtc, &prime_scanout->direct_fbs.front);
    ms_prime_free_cover(&prime_scanout->overlay);
    ms_overlay_destroy(crtc);
}

Bool
ms_prime_create_scanout(xf86CrtcPtr crtc, PixmapPtr ppix)
{
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    BoxRec whole = { 0, 0, ppix->drawable.width, ppix->drawable.height };
    int i;

    for (i = 0; i < ARRAY_SIZE(prime_scanout->buf); i++) {
        prime_scanout->buf[i].px =
            drmmode_shadow_fb_create(crtc, NULL, ppix->drawable.width,
                                     ppix->drawable.height,
                                     ppix->drawable.depth,
                                     &prime_scanout->buf[i].bo,
                                     &prime_scanout->buf[i].fb_id);
        if (!prime_scanout->buf[i].px) {
            ms_prime_destroy_scanout(crtc);
            return FALSE;
        }
        drmmode_clear_pixmap(prime_scanout->buf[i].px);
        RegionInit(&prime_scanout->buf[i].dmg, &whole, 1);
    }
    return TRUE;
}

static void
ms_prime_set_scanout_content(xf86CrtcPtr crtc, Bool with_next_flip)
{
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_prime_direct_rec *direct = &prime_scanout->direct;

    if (direct->source)
        drmmode_crtc_set_scanout_content(crtc, direct->pq,
                                         direct->has_metadata ?
                                         &direct->metadata : NULL,
                                         with_next_flip);
    else
        drmmode_crtc_set_scanout_content(crtc, prime_scanout->composition.pq,
                                         NULL, with_next_flip);
}

static msPixmapPrivPtr
ms_prime_source_priv(PixmapPtr ppix)
{
    PixmapPtr primary = ppix ? ppix->primary_pixmap : NULL;
    ScrnInfoPtr scrn;

    if (!primary)
        return NULL;

    scrn = xf86ScreenToScrn(primary->drawable.pScreen);
    if (!ms_scrn_is_modesetting(scrn))
        return NULL;
    return msGetPixmapPriv(&modesettingPTR(scrn)->drmmode, primary);
}

/* Only the back shared pixmap is painted, and everything drawn so far is in
 * it, so the damage of the other one is never needed.
 */
static void
ms_prime_discard_front_damage(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    msPixmapPrivPtr ppriv = ms_prime_source_priv(drmmode_crtc->prime_pixmap);

    if (ppriv && ppriv->dirty)
        DamageEmpty(ppriv->dirty->damage);
}

/* The shared pixmap is complete, so the back buffer only needs what changed
 * since it was last shown. The client's own buffer is new every frame.
 */
static RegionPtr
ms_prime_back_damage(xf86CrtcPtr crtc, PixmapPtr ppix, Bool shared)
{
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_shadow_fb_ptr back = &prime_scanout->buf[prime_scanout->back_idx];
    msPixmapPrivPtr ppriv = ms_prime_source_priv(ppix);
    BoxRec whole = { 0, 0, ppix->drawable.width, ppix->drawable.height };
    int i;

    if (!shared || !ppriv) {
        RegionReset(&back->dmg, &whole);
        return &back->dmg;
    }

    for (i = 0; i < ARRAY_SIZE(prime_scanout->buf); i++)
        RegionUnion(&prime_scanout->buf[i].dmg, &prime_scanout->buf[i].dmg,
                    &ppriv->painted);
    RegionEmpty(&ppriv->painted);
    return &back->dmg;
}

static uint32_t
ms_prime_copy_to_back(xf86CrtcPtr crtc, PixmapPtr src, RegionPtr damage,
                      int *fence_fd)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_shadow_fb_ptr back = &prime_scanout->buf[prime_scanout->back_idx];
    BoxPtr box = RegionRects(damage);
    int nbox = RegionNumRects(damage);
    GCPtr gc;

    gc = GetScratchGC(back->px->drawable.depth, screen);
    if (!gc)
        return 0;

    ValidateGC(&back->px->drawable, gc);
    for (; nbox--; box++)
        gc->ops->CopyArea(&src->drawable, &back->px->drawable, gc,
                          box->x1, box->y1, box->x2 - box->x1,
                          box->y2 - box->y1, box->x1, box->y1);
    FreeScratchGC(gc);
    RegionEmpty(damage);

    if (drmmode_crtc_can_fence_flip(crtc))
        *fence_fd = ms->glamor.egl_create_fence_fd(screen);
    if (*fence_fd < 0)
        ms->glamor.finish(screen);
    return back->fb_id;
}

static drmmode_flip_kind
ms_prime_flip_kind(const drmmode_prime_direct_rec *direct)
{
    if (!direct->source)
        return DRMMODE_FLIP_COMPOSED;
    return direct->scanout_source ? DRMMODE_FLIP_SCANOUT : DRMMODE_FLIP_COPY;
}

static uint64_t
ms_prime_frame_period(xf86CrtcPtr crtc)
{
    double refresh = xf86ModeVRefresh(&crtc->mode);

    return refresh > 0.0 ? 1000000.0 / refresh : 0;
}

static void
ms_prime_flip_target(xf86CrtcPtr crtc, drmmode_flip_kind kind)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_commit_lead_rec *lead = &drmmode_crtc->tearfree.compose.lead;
    uint64_t ust, msc;

    lead->kind = kind;
    lead->commit_ust = GetTimeInMicros();
    if (ms_get_crtc_ust_msc(crtc, &ust, &msc) != Success) {
        lead->target_msc = 0;
        return;
    }
    lead->target_msc = msc + 1;
    lead->target_ust = ust + ms_prime_frame_period(crtc);
}

/* A flip that missed its vblank although it was ready a full lead ahead of it
 * needs a longer lead. Misses caused by the composition finishing late are
 * covered by the composition time estimate instead.
 */
static void
ms_prime_flip_timing(xf86CrtcPtr crtc, uint64_t msc, uint64_t ready)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_commit_lead_rec *lead = &drmmode_crtc->tearfree.compose.lead;
    uint64_t *usec = &lead->usec[lead->kind];
    uint64_t period = ms_prime_frame_period(crtc);
    Bool late;

    if (!lead->target_msc || !period)
        return;

    late = msc > lead->target_msc && ready < lead->target_ust &&
        lead->target_ust - ready >= *usec;
    lead->late_history = (lead->late_history << 1) | late;
    if (__builtin_popcount(lead->late_history) < 2)
        return;

    lead->late_history = 0;
    *usec = min(*usec + 1000, period);
}

static void
ms_prime_flip_timing_done(xf86CrtcPtr crtc, uint64_t msc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_compose_timing_rec *compose = &drmmode_crtc->tearfree.compose;
    Bool fenced = compose->fence_fd >= 0;
    uint64_t signaled = ms_compose_timing_release(compose, TRUE);

    if (!fenced || signaled)
        ms_prime_flip_timing(crtc, msc, max(compose->lead.commit_ust, signaled));
}

Bool
ms_prime_flip(PixmapPtr ppix, xf86CrtcPtr crtc, uint32_t seq)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_prime_direct_rec *direct = &prime_scanout->direct;
    PixmapPtr src = direct->source ? direct->source : ppix;
    drmmode_prime_fb_rec fb = { 0 };
    uint32_t fb_id;
    int fence_fd = -1;
    int ret;

    if (direct->covered)
        ms_prime_overlay_prepare(crtc);
    ms_prime_discard_front_damage(crtc);

    if (direct->scanout_source &&
        ms_import_flip_fb(screen, src, &fb.fb_id, "PRIME direct-scanout")) {
        fb.pixmap = src;
        src->refcnt++;
        fb_id = fb.fb_id;
    } else {
        ms_prime_release_fb(crtc, &fb);
        fb_id = ms_prime_copy_to_back(crtc, src,
                                      ms_prime_back_damage(crtc, ppix,
                                                           !direct->source),
                                      &fence_fd);
        if (!fb_id) {
            ms_overlay_flip_failed(crtc);
            return FALSE;
        }
    }

    ms_prime_set_scanout_content(crtc, TRUE);
    drmmode_crtc->tearfree.compose.fence_fd = fence_fd >= 0 ? dup(fence_fd) : -1;
    ret = drmmode_crtc_flip(crtc, fb_id, 0, 0, DRM_MODE_PAGE_FLIP_EVENT,
                            fence_fd, (void *)(intptr_t) seq);
    if (fence_fd >= 0)
        close(fence_fd);
    if (ret) {
        ms_compose_timing_release(&drmmode_crtc->tearfree.compose, FALSE);
        ms_overlay_flip_failed(crtc);
        ms_prime_release_fb(crtc, &fb);
        return FALSE;
    }

    prime_scanout->direct_fbs.pending = fb;
    drmmode_crtc->tearfree.flip_seq = seq;
    ms_prime_flip_target(crtc, ms_prime_flip_kind(direct));
    return TRUE;
}

void
ms_prime_flip_done(xf86CrtcPtr crtc, uint64_t msc, uint64_t usec)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_prime_direct_fbs_rec *fbs = &prime_scanout->direct_fbs;

    ms_prime_flip_timing_done(crtc, msc);
    if (!fbs->pending.fb_id)
        prime_scanout->back_idx ^= 1;
    ms_overlay_flip_done(crtc);
    ms_prime_release_fb(crtc, &fbs->front);
    fbs->front = fbs->pending;
    fbs->pending = (drmmode_prime_fb_rec) { 0 };

    drmmode_crtc->tearfree.flip_seq = 0;
    ms_tearfree_dri_flipped(crtc, msc, usec);
}

void
ms_prime_flip_aborted(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    ms_prime_release_fb(crtc, &ms_prime_scanout(crtc)->direct_fbs.pending);
    ms_compose_timing_release(&drmmode_crtc->tearfree.compose, FALSE);
    ms_overlay_flip_aborted(crtc);
    drmmode_crtc->tearfree.flip_seq = 0;
    ms_tearfree_dri_abort_all(crtc);
}

xf86CrtcPtr
ms_prime_crtc(PixmapPtr ppix)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(ppix->drawable.pScreen);
    xf86CrtcConfigPtr config;
    int i;

    if (!ms_scrn_is_modesetting(scrn))
        return NULL;

    config = XF86_CRTC_CONFIG_PTR(scrn);
    for (i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        if (drmmode_crtc->prime_scanout.buf[0].px &&
            (drmmode_crtc->prime_pixmap == ppix ||
             drmmode_crtc->prime_pixmap_back == ppix))
            return crtc;
    }
    return NULL;
}

Bool
ms_prime_crtc_flipping(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc;

    if (!ms_scrn_is_modesetting(crtc->scrn))
        return FALSE;

    drmmode_crtc = crtc->driver_private;
    return drmmode_crtc->flipping_active &&
        drmmode_crtc->prime_scanout.buf[0].px;
}

static Bool
ms_prime_idle(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return msGetPixmapPriv(drmmode_crtc->drmmode,
                           drmmode_crtc->prime_pixmap_back)->wait_for_damage;
}

void
ms_prime_kick(xf86CrtcPtr crtc)
{
    if (ms_prime_idle(crtc))
        ms_prime_scanout(crtc)->kick = TRUE;
}

static void
ms_prime_present(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_compose_timing_rec *compose = &drmmode_crtc->tearfree.compose;
    msPixmapPrivPtr ppriv = msGetPixmapPriv(drmmode_crtc->drmmode,
                                            drmmode_crtc->prime_pixmap_back);
    uint64_t start = GetTimeInMicros();

    compose->start = start;
    ppriv->wait_for_damage = FALSE;
    if (!drmmode_SharedPixmapPresentNow(drmmode_crtc->prime_pixmap_back, crtc,
                                        drmmode_crtc->drmmode))
        ppriv->wait_for_damage = TRUE;

    if (drmmode_crtc->tearfree.flip_seq)
        ms_compose_timing_measured(compose, GetTimeInMicros() - start);
}

void
ms_prime_flush_kicks(ScreenPtr screen, void *timeout)
{
    PixmapDirtyUpdatePtr dirty;

    xorg_list_for_each_entry(dirty, &screen->pixmap_dirty_list, ent) {
        xf86CrtcPtr crtc = ms_prime_crtc(dirty->secondary_dst);
        drmmode_prime_scanout_rec *prime_scanout;

        if (!crtc)
            continue;

        prime_scanout = ms_prime_scanout(crtc);
        if (!prime_scanout->kick)
            continue;

        if (!ms_prime_idle(crtc)) {
            prime_scanout->kick = FALSE;
            continue;
        }

        if (!ms_tearfree_compose_due(crtc, timeout))
            continue;

        prime_scanout->kick = FALSE;
        ms_prime_present(crtc);
    }
}

static Bool
ms_prime_crtc_update_output_mode(xf86CrtcPtr crtc)
{
    if (!ms_prime_crtc_flipping(crtc))
        return TRUE;

    if (!ms_prime_idle(crtc))
        return FALSE;

    ms_prime_set_scanout_content(crtc, FALSE);
    ms_prime_kick(crtc);
    return TRUE;
}

Bool
ms_prime_update_output_mode(ScrnInfoPtr scrn)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    Bool updated = TRUE;
    int i;

    for (i = 0; i < config->num_crtc; i++)
        updated &= ms_prime_crtc_update_output_mode(config->crtc[i]);
    return updated;
}

static PixmapPtr
ms_prime_pixmap_from_fd(ScreenPtr screen, PixmapPtr pixmap, int fd,
                        CARD16 stride)
{
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(screen));
    PixmapPtr imported;

    imported = screen->CreatePixmap(screen, 0, 0, pixmap->drawable.depth,
                                    CREATE_PIXMAP_USAGE_SHARED);
    if (imported &&
        !ms->glamor.back_pixmap_from_fd(imported, fd, pixmap->drawable.width,
                                        pixmap->drawable.height, stride,
                                        pixmap->drawable.depth,
                                        pixmap->drawable.bitsPerPixel)) {
        dixDestroyPixmap(imported, 0);
        imported = NULL;
    }
    return imported;
}

static PixmapPtr
ms_prime_import_pixmap(xf86CrtcPtr crtc, PixmapPtr pixmap)
{
    ScreenPtr src_screen = pixmap->drawable.pScreen;
    ScrnInfoPtr src_scrn = xf86ScreenToScrn(src_screen);
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    PixmapPtr imported = ms_offload_native_pixmap(pixmap);
    struct gbm_bo *bo;
    int fd;

    if (imported && imported->drawable.pScreen == screen) {
        imported->refcnt++;
        return imported;
    }

    if (!ms_scrn_is_modesetting(src_scrn) ||
        !modesettingPTR(src_scrn)->drmmode.glamor)
        return NULL;

    bo = modesettingPTR(src_scrn)->glamor.gbm_bo_from_pixmap(src_screen,
                                                              pixmap);
    if (!bo)
        return NULL;

    imported = NULL;
    if (gbm_bo_get_modifier(bo) == DRM_FORMAT_MOD_LINEAR &&
        gbm_bo_get_plane_count(bo) == 1 && gbm_bo_get_offset(bo, 0) == 0 &&
        (fd = gbm_bo_get_fd(bo)) >= 0) {
        imported = ms_prime_pixmap_from_fd(screen, pixmap, fd,
                                           gbm_bo_get_stride(bo));
        close(fd);
    }

    gbm_bo_destroy(bo);
    return imported;
}

PixmapPtr
ms_prime_import(xf86CrtcPtr crtc, PixmapPtr pixmap)
{
    drmmode_prime_direct_rec *direct = &ms_prime_scanout(crtc)->direct;
    drmmode_prime_import_rec import;
    int last = ARRAY_SIZE(direct->imports) - 1;
    int i;

    for (i = 0; i < last && direct->imports[i].source != pixmap; i++)
        ;

    if (direct->imports[i].source == pixmap) {
        import = direct->imports[i];
    } else {
        ms_prime_release_import(&direct->imports[last]);
        i = last;
        pixmap->refcnt++;
        import.source = pixmap;
        import.imported = ms_prime_import_pixmap(crtc, pixmap);
    }

    memmove(&direct->imports[1], &direct->imports[0],
            i * sizeof(direct->imports[0]));
    direct->imports[0] = import;
    return import.imported;
}

static Bool
ms_prime_create_cover(xf86CrtcPtr crtc, ScreenPtr screen, int width,
                      int height)
{
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(screen));
    drmmode_prime_overlay_rec *overlay = &ms_prime_scanout(crtc)->overlay;
    PixmapPtr cover;
    CARD16 stride;
    CARD32 size;
    int fd;

    cover = screen->CreatePixmap(screen, width, height, 32,
                                 CREATE_PIXMAP_USAGE_SHARED);
    if (!cover)
        return FALSE;

    fd = ms->glamor.shareable_fd_from_pixmap(screen, cover, &stride, &size);
    if (fd >= 0)
        overlay->import = ms_prime_pixmap_from_fd(xf86ScrnToScreen(crtc->scrn),
                                                  cover, fd, stride);

    if (!overlay->import) {
        if (fd >= 0)
            close(fd);
        dixDestroyPixmap(cover, 0);
        return FALSE;
    }

    overlay->cover = cover;
    overlay->dmabuf_fd = fd;
    return TRUE;
}

drmmode_prime_overlay_rec *
ms_prime_overlay_cover(xf86CrtcPtr crtc, ScreenPtr screen, int width,
                       int height)
{
    drmmode_prime_overlay_rec *overlay = &ms_prime_scanout(crtc)->overlay;

    if (overlay->cover &&
        (overlay->cover->drawable.width != width ||
         overlay->cover->drawable.height != height))
        ms_prime_free_cover(overlay);

    if (!overlay->cover &&
        !ms_prime_create_cover(crtc, screen, width, height))
        return NULL;
    return overlay;
}
#endif
