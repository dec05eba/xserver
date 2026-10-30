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

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <pixman.h>
#include <xf86.h>
#include <xf86Crtc.h>
#include <compositeext.h>
#include <cursorstr.h>
#include <picturestr.h>
#include <present.h>
#include <servermd.h>

#include "driver.h"

typedef struct {
    const struct pixman_f_transform *to_screen;
    const struct pixman_f_transform *from_screen;
    PictFilterPtr filter;
    xFixed *params;
    int nparams;
    int filter_width;
    int filter_height;
} ms_compositor_transform_rec;

typedef struct {
    PixmapPtr dst;
    int x, y;
    RegionRec copied;
} ms_compositor_pq_rec;

typedef struct {
    int sdr_white_level;
    ms_compositor_pq_rec *pq;
} ms_compositor_paint_rec;

typedef struct {
    ScrnInfoPtr scrn;
    BoxRec bounds;
    drmmode_composition_rec *composition;
    const struct drm_color_ctm *user_ctm;
} ms_compositor_target_rec;

static Bool
ms_compositor_update_composition(drmmode_composition_rec *composition, Bool pq,
                                 int sdr_white_level)
{
    if (composition->pq == pq &&
        composition->sdr_white_level == sdr_white_level)
        return FALSE;

    composition->pq = pq;
    composition->sdr_white_level = sdr_white_level;
    if (composition->intermediate) {
        dixDestroyPixmap(composition->intermediate, 0);
        composition->intermediate = NULL;
    }
    return TRUE;
}

static void
ms_compositor_crtc_target(xf86CrtcPtr crtc, ms_compositor_target_rec *target)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    target->scrn = crtc->scrn;
    target->bounds = crtc->bounds;
    target->composition = &drmmode_crtc->tearfree.composition;
    target->user_ctm = drmmode_crtc_user_ctm(crtc);
}

static PicturePtr
ms_compositor_create_picture(ScreenPtr screen, PixmapPtr pixmap)
{
    PictFormatPtr format = pixmap->drawable.depth == 30 ?
        PictureMatchFormat(screen, 30, PICT_x2r10g10b10) :
        PictureWindowFormat(screen->root);
    int error;

    if (!format)
        return NULL;

    return CreatePicture(0, &pixmap->drawable, format, 0, NULL, serverClient,
                         &error);
}

#ifdef HAVE_GBM
static void
ms_compositor_subtract_cursors(ScrnInfoPtr scrn, RegionPtr region)
{
    modesettingPtr ms = modesettingPTR(scrn);
    ms_sw_cursor_ptr sw_cursor;

    xorg_list_for_each_entry(sw_cursor, &ms->compositor.sw_cursors, entry) {
        RegionRec cursor;

        RegionInit(&cursor, &sw_cursor->box, 1);
        RegionSubtract(region, region, &cursor);
        RegionUninit(&cursor);
    }
}

static void
ms_compositor_copy_pq(ScrnInfoPtr scrn, ms_compositor_pq_rec *pq,
                      WindowPtr window, RegionPtr exposed)
{
    ScreenPtr screen = scrn->pScreen;
    PixmapPtr src = screen->GetWindowPixmap(window);
    PicturePtr src_picture, dst_picture;
    RegionRec copy;
    int error;

    RegionNull(&copy);
    RegionCopy(&copy, exposed);
    ms_compositor_subtract_cursors(scrn, &copy);
    if (RegionNil(&copy)) {
        RegionUninit(&copy);
        return;
    }

    src_picture = CreatePicture(0, &src->drawable, PictureWindowFormat(window),
                                0, NULL, serverClient, &error);
    dst_picture = ms_compositor_create_picture(screen, pq->dst);
    if (src_picture && dst_picture) {
        SetPictureClipRegion(dst_picture, -pq->x, -pq->y, &copy);
        CompositePicture(PictOpSrc, src_picture, NULL, dst_picture,
                         0, 0, 0, 0, src->screen_x - pq->x,
                         src->screen_y - pq->y, src->drawable.width,
                         src->drawable.height);
        RegionUnion(&pq->copied, &pq->copied, &copy);
    }

    if (dst_picture)
        FreePicture(dst_picture, 0);
    if (src_picture)
        FreePicture(src_picture, 0);
    RegionUninit(&copy);
}

static Bool
ms_compositor_paint_hdr_window(PicturePtr dst, int x, int y, WindowPtr window,
                               RegionPtr region, RegionPtr exposed,
                               CARD16 opacity, void *closure)
{
    ScreenPtr screen = window->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    ms_compositor_paint_rec *paint = closure;
    PixmapPtr src = screen->GetWindowPixmap(window);
    PictFormatPtr format = PictureWindowFormat(window);
    glamor_color_conversion conversion;
    RegionRec dst_region;
    Bool painted;

    if (dst->pDrawable->type != DRAWABLE_PIXMAP)
        return FALSE;

    ms_hdr_pq_to_sdr_conversion(ms_hdr_window_metadata(window),
                                paint ? paint->sdr_white_level :
                                ms->drmmode.sdr_white_level_default,
                                &conversion);

    RegionNull(&dst_region);
    RegionCopy(&dst_region, region);
    if (paint && paint->pq) {
        ms_compositor_copy_pq(scrn, paint->pq, window, exposed);
        RegionSubtract(&dst_region, &dst_region, &paint->pq->copied);
    }
    RegionTranslate(&dst_region, -x, -y);
    painted = ms->glamor.convert_color(src, (PixmapPtr) dst->pDrawable,
                                       &dst_region, x - src->screen_x,
                                       y - src->screen_y,
                                       format && format->type == PictTypeDirect &&
                                       format->direct.alphaMask,
                                       opacity, &conversion);
    RegionUninit(&dst_region);
    return painted;
}
#endif

static CompositeServerPaintHdrProcPtr
ms_compositor_paint_hdr_proc(ScrnInfoPtr scrn)
{
#ifdef HAVE_GBM
    if (modesettingPTR(scrn)->glamor.convert_color)
        return ms_compositor_paint_hdr_window;
#endif
    return NULL;
}

static void
ms_compositor_enable_disable_fb_access(ScrnInfoPtr scrn, Bool enable)
{
    modesettingPtr ms = modesettingPTR(scrn);

    if (!enable)
        ms->compositor.EnableDisableFBAccess(scrn, FALSE);
    else if (!xf86ServerIsResetting())
        SetRootClip(scrn->pScreen, ROOT_CLIP_INPUT_ONLY);
}

void
ms_compositor_screen_init(ScrnInfoPtr scrn)
{
    modesettingPtr ms = modesettingPTR(scrn);

    xorg_list_init(&ms->compositor.sw_cursors);
    ms->compositor.EnableDisableFBAccess = scrn->EnableDisableFBAccess;
    scrn->EnableDisableFBAccess = ms_compositor_enable_disable_fb_access;
}

Bool
ms_compositor_create_screen_resources(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    PixmapPtr old_pixmap = screen->GetScreenPixmap(screen);
    PixmapPtr pixmap;

    pixmap = screen->CreatePixmap(screen, 0, 0, screen->rootDepth, 0);
    if (!pixmap)
        return FALSE;

    screen->SetScreenPixmap(pixmap);
    dixDestroyPixmap(old_pixmap, 0);

    if (!CompositeServerInit(screen, ms_compositor_paint_hdr_proc(scrn))) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "Failed to set up compositing, use "
                   "Option \"Compositing\" \"false\" for a shared framebuffer\n");
        return FALSE;
    }

    return TRUE;
}

static void
ms_compositor_add_damage(ScrnInfoPtr scrn, RegionPtr damage)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    int c, i;

    for (c = 0; c < config->num_crtc; c++) {
        xf86CrtcPtr crtc = config->crtc[c];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
        drmmode_tearfree_ptr trf = &drmmode_crtc->tearfree;
        RegionRec region;

        if (!trf->buf[0].px)
            continue;

        RegionInit(&region, &crtc->bounds, 1);
        RegionIntersect(&region, &region, damage);
        for (i = 0; i < ARRAY_SIZE(trf->buf); i++)
            RegionUnion(&trf->buf[i].dmg, &trf->buf[i].dmg, &region);
        RegionUninit(&region);
    }
}

static void
ms_compositor_damage_box(ScrnInfoPtr scrn, BoxPtr box)
{
    PixmapDirtyUpdatePtr dirty;
    RegionRec region;

    RegionInit(&region, box, 1);
    ms_compositor_add_damage(scrn, &region);
    xorg_list_for_each_entry(dirty, &scrn->pScreen->pixmap_dirty_list, ent)
        RegionUnion(DamageRegion(dirty->damage), DamageRegion(dirty->damage),
                    &region);
    RegionUninit(&region);
}

void
ms_compositor_update_damages(ScreenPtr screen)
{
    RegionRec damage;

    RegionNull(&damage);
    CompositeServerFlush(screen, &damage);
    if (RegionNotEmpty(&damage))
        ms_compositor_add_damage(xf86ScreenToScrn(screen), &damage);
    RegionUninit(&damage);
}

static void
ms_compositor_paint_cursors(ScrnInfoPtr scrn, PicturePtr dst, int x, int y,
                            RegionPtr region)
{
    modesettingPtr ms = modesettingPTR(scrn);
    ms_sw_cursor_ptr sw_cursor;

    xorg_list_for_each_entry(sw_cursor, &ms->compositor.sw_cursors, entry) {
        BoxPtr box = &sw_cursor->box;
        RegionRec clip;

        RegionInit(&clip, box, 1);
        RegionIntersect(&clip, &clip, region);
        if (RegionNotEmpty(&clip)) {
            SetPictureClipRegion(dst, -x, -y, &clip);
            CompositePicture(PictOpOver, sw_cursor->picture, NULL, dst,
                             0, 0, 0, 0, box->x1 - x, box->y1 - y,
                             box->x2 - box->x1, box->y2 - box->y1);
        }
        RegionUninit(&clip);
    }

    SetPictureClipRegion(dst, 0, 0, NULL);
}

static void
ms_compositor_paint_layers(ScrnInfoPtr scrn, PicturePtr dst, int x, int y,
                           RegionPtr region, ms_compositor_paint_rec *paint)
{
    CompositeServerPaint(scrn->pScreen, dst, x, y, region, paint);
    ms_compositor_paint_cursors(scrn, dst, x, y, region);
}

static PixmapPtr
ms_compositor_intermediate_pixmap(ms_compositor_target_rec *target,
                                  Bool *created)
{
    ScreenPtr screen = target->scrn->pScreen;
    drmmode_composition_rec *composition = target->composition;
    int width = target->bounds.x2 - target->bounds.x1;
    int height = target->bounds.y2 - target->bounds.y1;

    *created = FALSE;
    if (composition->intermediate &&
        composition->intermediate->drawable.width == width &&
        composition->intermediate->drawable.height == height)
        return composition->intermediate;

    if (composition->intermediate)
        dixDestroyPixmap(composition->intermediate, 0);

    composition->intermediate = screen->CreatePixmap(screen, width, height,
                                                     screen->rootDepth, 0);
    *created = composition->intermediate != NULL;
    return composition->intermediate;
}

static PixmapPtr
ms_compositor_paint_intermediate(ms_compositor_target_rec *target,
                                 RegionPtr region,
                                 ms_compositor_paint_rec *paint)
{
    ScrnInfoPtr scrn = target->scrn;
    PixmapPtr pixmap;
    PicturePtr picture;
    RegionRec bounds;
    Bool created;

    pixmap = ms_compositor_intermediate_pixmap(target, &created);
    if (!pixmap)
        return NULL;

    picture = ms_compositor_create_picture(scrn->pScreen, pixmap);
    if (!picture)
        return NULL;

    RegionInit(&bounds, &target->bounds, 1);
    ms_compositor_paint_layers(scrn, picture, target->bounds.x1,
                               target->bounds.y1, created ? &bounds : region,
                               paint);
    RegionUninit(&bounds);
    FreePicture(picture, 0);
    return pixmap;
}

static void
ms_compositor_copy_transformed(PicturePtr src, int x, int y, PicturePtr dst,
                               RegionPtr region,
                               const ms_compositor_transform_rec *transform)
{
    struct pixman_f_transform f_transform = *transform->to_screen;
    BoxPtr box = RegionRects(region);
    int n = RegionNumRects(region);
    PictTransform pict_transform;

    pixman_f_transform_translate(&f_transform, NULL, -x, -y);
    if (!pixman_transform_from_pixman_f_transform(&pict_transform,
                                                  &f_transform) ||
        SetPictureTransform(src, &pict_transform) != Success)
        return;

    if (transform->filter)
        SetPicturePictFilter(src, transform->filter, transform->params,
                             transform->nparams);

    while (n--) {
        BoxRec dst_box = *box++;

        dst_box.x1 -= transform->filter_width >> 1;
        dst_box.x2 += transform->filter_width >> 1;
        dst_box.y1 -= transform->filter_height >> 1;
        dst_box.y2 += transform->filter_height >> 1;
        pixman_f_transform_bounds(transform->from_screen, &dst_box);
        CompositePicture(PictOpSrc, src, NULL, dst,
                         dst_box.x1, dst_box.y1, 0, 0, dst_box.x1, dst_box.y1,
                         dst_box.x2 - dst_box.x1, dst_box.y2 - dst_box.y1);
    }
}

static void
ms_compositor_paint_transformed(xf86CrtcPtr crtc,
                                ms_compositor_target_rec *target,
                                PicturePtr dst, RegionPtr region,
                                ms_compositor_paint_rec *paint)
{
    ScrnInfoPtr scrn = crtc->scrn;
    ms_compositor_transform_rec transform = {
        .to_screen = &crtc->f_crtc_to_framebuffer,
        .from_screen = &crtc->f_framebuffer_to_crtc,
        .filter = crtc->filter,
        .params = crtc->params,
        .nparams = crtc->nparams,
        .filter_width = crtc->filter_width,
        .filter_height = crtc->filter_height,
    };
    PixmapPtr pixmap;
    PicturePtr src;

    pixmap = ms_compositor_paint_intermediate(target, region, paint);
    if (!pixmap)
        return;

    src = ms_compositor_create_picture(scrn->pScreen, pixmap);
    if (!src)
        return;

    ms_compositor_copy_transformed(src, crtc->bounds.x1, crtc->bounds.y1, dst,
                                   region, &transform);
    FreePicture(src, 0);
}

#ifdef HAVE_GBM
static void
ms_compositor_paint_pq(ms_compositor_target_rec *target, PixmapPtr dst,
                       RegionPtr region, ms_compositor_paint_rec *paint)
{
    modesettingPtr ms = modesettingPTR(target->scrn);
    ms_compositor_pq_rec pq = {
        .dst = dst,
        .x = target->bounds.x1,
        .y = target->bounds.y1,
    };
    glamor_color_conversion conversion;
    PixmapPtr pixmap;
    RegionRec converted;

    RegionNull(&pq.copied);
    paint->pq = &pq;
    pixmap = ms_compositor_paint_intermediate(target, region, paint);
    paint->pq = NULL;

    if (pixmap) {
        RegionNull(&converted);
        RegionSubtract(&converted, region, &pq.copied);
        RegionTranslate(&converted, -pq.x, -pq.y);
        ms_hdr_sdr_to_pq_conversion(target->user_ctm, paint->sdr_white_level,
                                    &conversion);
        ms->glamor.convert_color(pixmap, dst, &converted, 0, 0, FALSE, 0xffff,
                                 &conversion);
        RegionUninit(&converted);
    }
    RegionUninit(&pq.copied);
}
#endif

void
ms_compositor_paint(xf86CrtcPtr crtc, PixmapPtr dst, RegionPtr region)
{
    ScrnInfoPtr scrn = crtc->scrn;
    ms_compositor_target_rec target;
    ms_compositor_paint_rec paint;
    PicturePtr dst_picture;

    if (RegionNil(region))
        return;

    ms_compositor_crtc_target(crtc, &target);
    paint = (ms_compositor_paint_rec) {
        .sdr_white_level = target.composition->sdr_white_level,
    };

#ifdef HAVE_GBM
    if (target.composition->pq) {
        ms_compositor_paint_pq(&target, dst, region, &paint);
        return;
    }
#endif

    dst_picture = ms_compositor_create_picture(scrn->pScreen, dst);
    if (!dst_picture)
        return;

    if (crtc->transform_in_use)
        ms_compositor_paint_transformed(crtc, &target, dst_picture, region,
                                        &paint);
    else
        ms_compositor_paint_layers(scrn, dst_picture, crtc->x, crtc->y,
                                   region, &paint);

    FreePicture(dst_picture, 0);
}

static void
ms_compositor_paint_rotated(ScrnInfoPtr scrn, PicturePtr dst,
                            PixmapDirtyUpdatePtr dirty, RegionPtr region)
{
    ScreenPtr screen = scrn->pScreen;
    BoxPtr extents = RegionExtents(region);
    ms_compositor_transform_rec transform = {
        .to_screen = &dirty->f_transform,
        .from_screen = &dirty->f_inverse,
    };
    PixmapPtr pixmap;
    PicturePtr src;

    pixmap = screen->CreatePixmap(screen, extents->x2 - extents->x1,
                                  extents->y2 - extents->y1,
                                  screen->rootDepth, 0);
    if (!pixmap)
        return;

    src = ms_compositor_create_picture(screen, pixmap);
    dixDestroyPixmap(pixmap, 0);
    if (!src)
        return;

    ms_compositor_paint_layers(scrn, src, extents->x1, extents->y1, region,
                               NULL);
    ms_compositor_copy_transformed(src, extents->x1, extents->y1, dst, region,
                                   &transform);
    FreePicture(src, 0);
}

static PixmapPtr
ms_compositor_dirty_dst(PixmapDirtyUpdatePtr dirty)
{
    return dirty->secondary_dst->primary_pixmap ?
        dirty->secondary_dst->primary_pixmap : dirty->secondary_dst;
}

static void
ms_compositor_dirty_box(PixmapDirtyUpdatePtr dirty, BoxPtr box)
{
    PixmapPtr dst = ms_compositor_dirty_dst(dirty);
    Bool rotated = dirty->rotation == RR_Rotate_90 ||
        dirty->rotation == RR_Rotate_270;

    box->x1 = dirty->x;
    box->y1 = dirty->y;
    box->x2 = dirty->x + (rotated ? dst->drawable.height : dst->drawable.width);
    box->y2 = dirty->y + (rotated ? dst->drawable.width : dst->drawable.height);
}

static void
ms_compositor_prime_target(PixmapDirtyUpdatePtr dirty, xf86CrtcPtr crtc,
                           ms_compositor_target_rec *target)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    target->scrn = xf86ScreenToScrn(dirty->src->pScreen);
    ms_compositor_dirty_box(dirty, &target->bounds);
    target->composition = &drmmode_crtc->prime_scanout.composition;
    target->user_ctm = drmmode_crtc_user_ctm(crtc);
}

Bool
ms_compositor_sync_dirty(PixmapDirtyUpdatePtr dirty)
{
    ScreenPtr screen = dirty->src->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    PixmapPtr dst = ms_compositor_dirty_dst(dirty);
    xf86CrtcPtr crtc = drmmode_prime_scanout_crtc(dirty->secondary_dst);
    ms_compositor_paint_rec paint = {
        .sdr_white_level = modesettingPTR(scrn)->drmmode.sdr_white_level_default,
    };
    ms_compositor_target_rec target;
    PicturePtr dst_picture;
    Bool painted = FALSE;
    RegionRec region;
    BoxRec box;

    ms_compositor_dirty_box(dirty, &box);
    RegionInit(&region, &box, 1);
    RegionIntersect(&region, &region, DamageRegion(dirty->damage));
    if (RegionNil(&region)) {
        RegionUninit(&region);
        return FALSE;
    }

    if (crtc) {
        ms_compositor_prime_target(dirty, crtc, &target);
        paint.sdr_white_level = target.composition->sdr_white_level;
#ifdef HAVE_GBM
        if (target.composition->pq) {
            ms_compositor_paint_pq(&target, dst, &region, &paint);
            RegionUninit(&region);
            return TRUE;
        }
#endif
    }

    dst_picture = ms_compositor_create_picture(screen, dst);
    if (dst_picture) {
        if (dirty->rotation == RR_Rotate_0)
            ms_compositor_paint_layers(scrn, dst_picture,
                                       dirty->x - dirty->dst_x,
                                       dirty->y - dirty->dst_y, &region,
                                       &paint);
        else
            ms_compositor_paint_rotated(scrn, dst_picture, dirty, &region);
        FreePicture(dst_picture, 0);
        painted = TRUE;
    }

    RegionUninit(&region);
    return painted;
}

#ifdef HAVE_GBM
static void
ms_compositor_damage_prime(ScreenPtr screen, xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    PixmapDirtyUpdatePtr dirty;

    xorg_list_for_each_entry(dirty, &screen->pixmap_dirty_list, ent) {
        RegionRec region;
        BoxRec box;

        if (dirty->secondary_dst != drmmode_crtc->prime_pixmap &&
            dirty->secondary_dst != drmmode_crtc->prime_pixmap_back)
            continue;

        ms_compositor_dirty_box(dirty, &box);
        RegionInit(&region, &box, 1);
        RegionUnion(DamageRegion(dirty->damage), DamageRegion(dirty->damage),
                    &region);
        RegionUninit(&region);
    }
}

void
ms_compositor_update_prime(PixmapDirtyUpdatePtr dirty)
{
    ScreenPtr screen = dirty->src->pScreen;
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(screen));
    xf86CrtcPtr crtc = drmmode_prime_scanout_crtc(dirty->secondary_dst);
    drmmode_crtc_private_ptr drmmode_crtc;
    Bool pq;

    if (!crtc)
        return;

    drmmode_crtc = crtc->driver_private;
    pq = ms->glamor.convert_color && dirty->rotation == RR_Rotate_0 &&
        !dirty->dst_x && !dirty->dst_y &&
        ms_compositor_dirty_dst(dirty)->drawable.depth == 30 &&
        drmmode_crtc_hdr_enabled(crtc);

    if (ms_compositor_update_composition(&drmmode_crtc->prime_scanout.composition,
                                         pq, drmmode_crtc_sdr_white_level(crtc)))
        ms_compositor_damage_prime(screen, crtc);
}
#endif

#ifdef HAVE_GBM
static Bool
ms_compositor_cursor_on_crtc(ScrnInfoPtr scrn, xf86CrtcPtr crtc)
{
    modesettingPtr ms = modesettingPTR(scrn);
    ms_sw_cursor_ptr sw_cursor;

    xorg_list_for_each_entry(sw_cursor, &ms->compositor.sw_cursors, entry) {
        BoxPtr box = &sw_cursor->box;

        if (box->x1 < crtc->bounds.x2 && box->x2 > crtc->bounds.x1 &&
            box->y1 < crtc->bounds.y2 && box->y2 > crtc->bounds.y1)
            return TRUE;
    }
    return FALSE;
}

static WindowPtr
ms_compositor_scanout_window(xf86CrtcPtr crtc, PixmapPtr *pixmap)
{
    ScrnInfoPtr scrn = crtc->scrn;
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_scanout_rec *scanout = &drmmode_crtc->tearfree.scanout;
    WindowPtr window;

    if (!ms->drmmode.pageflip || crtc->transform_in_use ||
        ms_compositor_cursor_on_crtc(scrn, crtc))
        return NULL;

    window = CompositeServerScanoutWindow(scrn->pScreen, &crtc->bounds);
    if (!window)
        return NULL;

    *pixmap = present_window_flip_pixmap(window);
    if (!*pixmap || *pixmap == scanout->failed_pixmap ||
        !ms_pixmap_scanout_format_supported(scrn->pScreen, *pixmap))
        return NULL;

    if (!drmmode_crtc_can_show_pq(crtc) &&
        CompositeServerHdrWindow(scrn->pScreen, &crtc->bounds))
        return NULL;

    return window;
}

static Bool
ms_compositor_window_has_variable_refresh(modesettingPtr ms, WindowPtr window)
{
    WindowPtr child;

    if (ms_window_has_variable_refresh(ms, window))
        return TRUE;

    for (child = window->firstChild; child; child = child->nextSib) {
        if (child->viewable &&
            ms_compositor_window_has_variable_refresh(ms, child))
            return TRUE;
    }
    return FALSE;
}

static Bool
ms_compositor_can_compose_pq(xf86CrtcPtr crtc)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return ms->glamor.convert_color && !crtc->transform_in_use &&
        drmmode_crtc->tearfree.buf[0].px->drawable.depth == 30 &&
        drmmode_crtc_hdr_enabled(crtc);
}

static void
ms_compositor_set_composition(xf86CrtcPtr crtc, Bool pq)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_tearfree_ptr trf = &drmmode_crtc->tearfree;
    int i;

    if (!ms_compositor_update_composition(&trf->composition, pq,
                                          drmmode_crtc_sdr_white_level(crtc)))
        return;

    for (i = 0; i < ARRAY_SIZE(trf->buf); i++)
        RegionReset(&trf->buf[i].dmg, &crtc->bounds);
}

static Bool
ms_compositor_update_output_mode(xf86CrtcPtr crtc, WindowPtr window)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_scanout_rec *scanout = &drmmode_crtc->tearfree.scanout;
    Bool updated;

    if (window == scanout->output_mode_window && !scanout->output_mode_dirty)
        return TRUE;

    if (window) {
        WindowPtr hdr_window = CompositeServerHdrWindow(crtc->scrn->pScreen,
                                                        &crtc->bounds);

        updated = drmmode_crtc_set_scanout_content(crtc, hdr_window != NULL,
            hdr_window ? ms_hdr_window_metadata(hdr_window) : NULL, TRUE);
    }
    else {
        Bool pq = ms_compositor_can_compose_pq(crtc);

        ms_compositor_set_composition(crtc, pq);
        updated = drmmode_crtc_set_scanout_content(crtc, pq, NULL, TRUE);
    }

    if (ms->vrr_support)
        drmmode_crtc_set_vrr(crtc, window &&
                             ms_compositor_window_has_variable_refresh(ms, window));

    scanout->output_mode_window = window;
    scanout->output_mode_dirty = !updated;
    return updated;
}

Bool
ms_compositor_direct_flip(xf86CrtcPtr crtc)
{
    ScrnInfoPtr scrn = crtc->scrn;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_scanout_rec *scanout = &drmmode_crtc->tearfree.scanout;
    Bool async = scanout->async;
    PixmapPtr pixmap = NULL;
    WindowPtr window;

    scanout->async = FALSE;

    window = ms_compositor_scanout_window(crtc, &pixmap);
    if (window && ms_compositor_update_output_mode(crtc, window)) {
        if (pixmap == scanout->pixmap && scanout->fb_id &&
            !drmmode_crtc_has_pending_hdr(crtc)) {
            ms_tearfree_dri_abort_all(crtc);
            return TRUE;
        }

        if (ms_do_direct_flip(scrn->pScreen, crtc, window, pixmap, async))
            return TRUE;

        scanout->failed_pixmap = pixmap;
    }

    ms_compositor_update_output_mode(crtc, NULL);
    return FALSE;
}
#endif

static Bool
ms_compositor_cursor_bit(const unsigned char *bits, int stride, int x, int y)
{
    unsigned char byte = bits[y * stride + x / 8];

#if BITMAP_BIT_ORDER == MSBFirst
    return byte & (0x80 >> (x & 7));
#else
    return byte & (1 << (x & 7));
#endif
}

static CARD32
ms_compositor_cursor_color(CARD16 red, CARD16 green, CARD16 blue)
{
    return 0xff000000 | (red >> 8) << 16 | (green >> 8) << 8 | blue >> 8;
}

static CARD32 *
ms_compositor_core_cursor_argb(CursorPtr cursor)
{
    CursorBitsPtr bits = cursor->bits;
    int stride = BitmapBytePad(bits->width);
    CARD32 fg = ms_compositor_cursor_color(cursor->foreRed, cursor->foreGreen,
                                           cursor->foreBlue);
    CARD32 bg = ms_compositor_cursor_color(cursor->backRed, cursor->backGreen,
                                           cursor->backBlue);
    CARD32 *argb;
    int x, y;

    argb = calloc(bits->width * bits->height, sizeof(*argb));
    if (!argb)
        return NULL;

    for (y = 0; y < bits->height; y++) {
        for (x = 0; x < bits->width; x++) {
            if (!ms_compositor_cursor_bit(bits->mask, stride, x, y))
                continue;
            argb[y * bits->width + x] =
                ms_compositor_cursor_bit(bits->source, stride, x, y) ? fg : bg;
        }
    }

    return argb;
}

static PicturePtr
ms_compositor_cursor_picture(ScreenPtr screen, CursorPtr cursor)
{
    CursorBitsPtr bits = cursor->bits;
    CARD32 *core_argb = bits->argb ? NULL : ms_compositor_core_cursor_argb(cursor);
    CARD32 *argb = bits->argb ? bits->argb : core_argb;
    PicturePtr picture = NULL;
    PixmapPtr pixmap;
    GCPtr gc;
    int error;

    if (!argb)
        return NULL;

    pixmap = screen->CreatePixmap(screen, bits->width, bits->height, 32, 0);
    gc = GetScratchGC(32, screen);
    if (pixmap && gc) {
        ValidateGC(&pixmap->drawable, gc);
        gc->ops->PutImage(&pixmap->drawable, gc, 32, 0, 0, bits->width,
                          bits->height, 0, ZPixmap, (char *) argb);
        picture = CreatePicture(0, &pixmap->drawable,
                                PictureMatchFormat(screen, 32, PICT_a8r8g8b8),
                                0, NULL, serverClient, &error);
    }

    if (gc)
        FreeScratchGC(gc);
    if (pixmap)
        dixDestroyPixmap(pixmap, 0);
    free(core_argb);
    return picture;
}

static void
ms_compositor_place_cursor(ms_sw_cursor_ptr sw_cursor, CursorPtr cursor,
                           int x, int y)
{
    sw_cursor->box.x1 = x - cursor->bits->xhot;
    sw_cursor->box.y1 = y - cursor->bits->yhot;
    sw_cursor->box.x2 = sw_cursor->box.x1 + cursor->bits->width;
    sw_cursor->box.y2 = sw_cursor->box.y1 + cursor->bits->height;
}

void
ms_compositor_set_cursor(ScrnInfoPtr scrn, ms_sw_cursor_ptr sw_cursor,
                         CursorPtr cursor, int x, int y)
{
    modesettingPtr ms = modesettingPTR(scrn);

    if (sw_cursor->listed) {
        ms_compositor_damage_box(scrn, &sw_cursor->box);
        xorg_list_del(&sw_cursor->entry);
        sw_cursor->listed = FALSE;
    }

    if (sw_cursor->picture) {
        FreePicture(sw_cursor->picture, 0);
        sw_cursor->picture = NULL;
    }

    if (!cursor)
        return;

    sw_cursor->picture = ms_compositor_cursor_picture(scrn->pScreen, cursor);
    if (!sw_cursor->picture)
        return;

    ms_compositor_place_cursor(sw_cursor, cursor, x, y);
    xorg_list_add(&sw_cursor->entry, &ms->compositor.sw_cursors);
    sw_cursor->listed = TRUE;
    ms_compositor_damage_box(scrn, &sw_cursor->box);
}

void
ms_compositor_move_cursor(ScrnInfoPtr scrn, ms_sw_cursor_ptr sw_cursor,
                          CursorPtr cursor, int x, int y)
{
    if (!sw_cursor->listed)
        return;

    ms_compositor_damage_box(scrn, &sw_cursor->box);
    ms_compositor_place_cursor(sw_cursor, cursor, x, y);
    ms_compositor_damage_box(scrn, &sw_cursor->box);
}
