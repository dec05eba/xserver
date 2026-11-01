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
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>
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

static void ms_prime_commit_cancel(xf86CrtcPtr crtc);
static void ms_prime_source_wait_cancel(xf86CrtcPtr crtc);
static void ms_prime_deadline_fired(int fd, int ready, void *data);

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

static Bool
ms_prime_timer_create(int *fd, NotifyFdProcPtr notify, xf86CrtcPtr crtc)
{
    if (*fd >= 0)
        return TRUE;

    *fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (*fd < 0)
        return FALSE;

    if (SetNotifyFd(*fd, notify, X_NOTIFY_READ, crtc))
        return TRUE;

    close(*fd);
    *fd = -1;
    return FALSE;
}

static void
ms_prime_timer_destroy(int *fd)
{
    if (*fd < 0)
        return;

    SetNotifyFd(*fd, NULL, 0, NULL);
    close(*fd);
    *fd = -1;
}

static void
ms_prime_timer_set(int fd, uint64_t *armed_at, uint64_t at)
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

static Bool
ms_prime_timer_expired(int fd)
{
    uint64_t expirations;

    return read(fd, &expirations, sizeof(expirations)) == sizeof(expirations);
}

static void
ms_prime_commit_reset(xf86CrtcPtr crtc)
{
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_prime_commit_rec *commit = &prime_scanout->commit;

    ms_prime_timer_destroy(&commit->deadline.fd);
    ms_prime_release_fb(crtc, &commit->flight.fb);
    *commit = (drmmode_prime_commit_rec) {
        .flight = { .buf_idx = -1 },
        .deadline = { .fd = -1 },
    };
    prime_scanout->presenters = (drmmode_prime_presenters_rec) { 0 };
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

    ms_prime_source_wait_cancel(crtc);
    ms_prime_commit_cancel(crtc);
    ms_prime_commit_reset(crtc);
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
    drmmode_prime_commit_rec *commit = &prime_scanout->commit;
    BoxRec whole = { 0, 0, ppix->drawable.width, ppix->drawable.height };
    int i;

    if (!ms_prime_timer_create(&commit->deadline.fd, ms_prime_deadline_fired,
                               crtc)) {
        ms_prime_commit_reset(crtc);
        return FALSE;
    }

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
    prime_scanout->back_idx = 0;
    prime_scanout->front_idx = ARRAY_SIZE(prime_scanout->buf) - 1;
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
    BoxPtr extents = RegionExtents(damage);
    GCPtr gc;

    gc = GetScratchGC(back->px->drawable.depth, screen);
    if (!gc)
        return 0;

    if (RegionNotEmpty(damage)) {
        gc->funcs->ChangeClip(gc, CT_REGION, RegionDuplicate(damage), 0);
        ValidateGC(&back->px->drawable, gc);
        gc->ops->CopyArea(&src->drawable, &back->px->drawable, gc,
                          extents->x1, extents->y1, extents->x2 - extents->x1,
                          extents->y2 - extents->y1, extents->x1, extents->y1);
    }
    FreeScratchGC(gc);
    RegionEmpty(damage);

    if (drmmode_crtc_can_fence_flip(crtc))
        *fence_fd = ms->glamor.egl_create_fence_fd(screen);
    if (*fence_fd < 0)
        ms->glamor.finish(screen);
    return back->fb_id;
}

static uint64_t
ms_prime_frame_period(xf86CrtcPtr crtc)
{
    double refresh = xf86ModeVRefresh(&crtc->mode);

    return refresh > 0.0 ? 1000000.0 / refresh : 0;
}

#define MS_PRIME_COMMIT_SLACK_USEC 1000
#define MS_PRIME_COMMIT_MAX_EXTRA_USEC 3000
#define MS_PRIME_COMMIT_RETRY_USEC 1000
#define MS_PRIME_COMMIT_LATE_CALLBACK_USEC 1000
#define MS_PRIME_COMMIT_STALLED_USEC 2000
#define MS_PRIME_PRESENTERS_WAIT_USEC 1000

static uint64_t
ms_prime_base_margin(xf86CrtcPtr crtc)
{
    return ms_crtc_vblank_usec(crtc) + MS_PRIME_COMMIT_SLACK_USEC;
}

static uint64_t
ms_prime_commit_margin(xf86CrtcPtr crtc)
{
    return ms_prime_base_margin(crtc) +
        ms_prime_scanout(crtc)->commit.extra_margin;
}

static void
ms_prime_commit_adapt(xf86CrtcPtr crtc, uint64_t vblank, uint64_t done)
{
    drmmode_prime_commit_rec *commit = &ms_prime_scanout(crtc)->commit;
    uint64_t wanted = vblank - min(vblank, ms_prime_base_margin(crtc));
    uint64_t limit = min(MS_PRIME_COMMIT_MAX_EXTRA_USEC,
                         ms_prime_frame_period(crtc) / 2);
    uint64_t extra = commit->extra_margin;

    if (done > wanted)
        extra += done - wanted;
    else
        extra -= min(extra, (wanted - done) / 10);
    commit->extra_margin = min(extra, limit);
}

static Bool
ms_prime_fence_signaled(int fence_fd)
{
    struct pollfd pfd = { .fd = fence_fd, .events = POLLIN };

    return fence_fd < 0 || poll(&pfd, 1, 0) != 0;
}

static Bool
ms_prime_cursor_wanted(xf86CrtcPtr crtc)
{
    return crtc->scrn->vtSema && xf86_crtc_on(crtc) &&
        ms_cursor_needs_commit(crtc);
}

static Bool
ms_prime_commit_wanted(xf86CrtcPtr crtc)
{
    drmmode_prime_commit_rec *commit = &ms_prime_scanout(crtc)->commit;

    return !commit->flight.seq &&
        (commit->queue.count || ms_prime_cursor_wanted(crtc));
}

static void ms_prime_commit_now(xf86CrtcPtr crtc, int64_t late);
static void ms_prime_frame_tick(xf86CrtcPtr crtc, uint64_t msc, uint64_t ust);
static void ms_prime_tick_start(xf86CrtcPtr crtc, uint32_t seq,
                                const drmmode_prime_deadline_rec *deadline);

/* Retries never commit from here, so a deadline that keeps being missed
 * moves on through the timer instead of recursing. A vblank gets at most one
 * commit, also when nvidia completes a flip before that vblank.
 */
static void
ms_prime_commit_schedule(xf86CrtcPtr crtc, uint64_t after)
{
    drmmode_prime_commit_rec *commit = &ms_prime_scanout(crtc)->commit;
    drmmode_prime_deadline_rec *deadline = &commit->deadline;
    uint64_t now = GetTimeInMicros();
    uint64_t period = ms_prime_frame_period(crtc);
    uint64_t from = max(now, after);
    uint64_t vblank, msc, due;

    if (commit->last_target)
        from = max(from, commit->last_target + period / 2);

    if (!ms_prime_commit_wanted(crtc)) {
        ms_prime_timer_set(deadline->fd, &deadline->at, 0);
        return;
    }

    if (!period || !ms_tearfree_next_vblank(crtc, from, &vblank, &msc)) {
        deadline->vblank = 0;
        deadline->msc = 0;
        deadline->on_time = FALSE;
        if (after) {
            ms_prime_timer_set(deadline->fd, &deadline->at, max(now, after));
            return;
        }
        ms_prime_timer_set(deadline->fd, &deadline->at, 0);
        ms_prime_commit_now(crtc, 0);
        return;
    }

    if (now + ms_crtc_vblank_usec(crtc) >= vblank) {
        vblank += period;
        msc++;
    }
    due = vblank - min(vblank, ms_prime_commit_margin(crtc));
    deadline->vblank = vblank;
    deadline->msc = msc;
    deadline->on_time = now < due;
    if (deadline->on_time || after) {
        ms_prime_timer_set(deadline->fd, &deadline->at, max(now, due));
        return;
    }

    ms_prime_timer_set(deadline->fd, &deadline->at, 0);
    ms_prime_commit_now(crtc, 0);
}

static drmmode_prime_content_rec *
ms_prime_queue_head(xf86CrtcPtr crtc)
{
    drmmode_prime_queue_rec *queue = &ms_prime_scanout(crtc)->commit.queue;

    return queue->count ? &queue->content[0] : NULL;
}

static drmmode_prime_content_rec *
ms_prime_queue_tail(xf86CrtcPtr crtc)
{
    drmmode_prime_queue_rec *queue = &ms_prime_scanout(crtc)->commit.queue;

    return queue->count ? &queue->content[queue->count - 1] : NULL;
}

static void
ms_prime_content_unwatch(drmmode_prime_content_rec *content)
{
    if (!content->watching_fence)
        return;

    SetNotifyFd(content->fence_fd, NULL, 0, NULL);
    content->watching_fence = FALSE;
}

static void
ms_prime_queue_drop(xf86CrtcPtr crtc, int index)
{
    drmmode_prime_queue_rec *queue = &ms_prime_scanout(crtc)->commit.queue;
    drmmode_prime_content_rec *content = &queue->content[index];

    ms_prime_content_unwatch(content);
    if (content->fence_fd >= 0)
        close(content->fence_fd);
    queue->count--;
    memmove(content, content + 1,
            (queue->count - index) * sizeof(queue->content[0]));
}

static Bool
ms_prime_queue_remove(xf86CrtcPtr crtc, uint32_t seq)
{
    drmmode_prime_queue_rec *queue = &ms_prime_scanout(crtc)->commit.queue;
    int i;

    for (i = 0; i < queue->count; i++) {
        if (queue->content[i].seq == seq) {
            ms_prime_queue_drop(crtc, i);
            return TRUE;
        }
    }
    return FALSE;
}

static void
ms_prime_commit_cancel(xf86CrtcPtr crtc)
{
    drmmode_prime_content_rec *head;

    while ((head = ms_prime_queue_head(crtc))) {
        uint32_t seq = head->seq;

        ms_prime_queue_drop(crtc, 0);
        ms_drm_abort_seq(crtc->scrn, seq);
    }
}

/* When frames for the output are late, it works one frame ahead: a frame's
 * clients go on once its copy is queued instead of once it is committed, so
 * the next frame is composed while this one waits for its vblank. A copy of a
 * client's own buffer reads it until it is done, so its clients go on then.
 */
static void
ms_prime_tick_early(xf86CrtcPtr crtc, drmmode_prime_content_rec *content)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_prime_commit_rec *commit = &ms_prime_scanout(crtc)->commit;
    drmmode_overlay_plane_rec *overlay = &drmmode_crtc->overlay_plane;
    uint64_t vblank, msc;

    if (!commit->triple.active || content->ticked || content->buf_idx < 0 ||
        commit->tick_seq || overlay->fb_id || overlay->buffers.pending ||
        (content->reads_client && !ms_prime_fence_signaled(content->fence_fd)) ||
        !ms_tearfree_next_vblank(crtc, GetTimeInMicros(), &vblank, &msc))
        return;

    content->ticked = TRUE;
    commit->tick_seq = content->seq;
    ms_prime_frame_tick(crtc, msc, vblank);
}

static void
ms_prime_commit_fence_signaled(int fd, int ready, void *data)
{
    xf86CrtcPtr crtc = data;
    drmmode_prime_queue_rec *queue = &ms_prime_scanout(crtc)->commit.queue;
    int i;

    for (i = 0; i < queue->count; i++) {
        if (queue->content[i].fence_fd == fd) {
            ms_prime_content_unwatch(&queue->content[i]);
            ms_prime_tick_early(crtc, &queue->content[i]);
        }
    }
    ms_prime_commit_schedule(crtc, 0);
}

static void
ms_prime_commit_queue(xf86CrtcPtr crtc, uint32_t seq, uint32_t fb_id,
                      int buf_idx, int fence_fd)
{
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_prime_commit_rec *commit = &prime_scanout->commit;
    drmmode_prime_queue_rec *queue = &commit->queue;
    drmmode_prime_content_rec *content;

    if (queue->count == ARRAY_SIZE(queue->content)) {
        uint32_t dropped = queue->content[0].seq;

        ms_prime_queue_drop(crtc, 0);
        ms_drm_abort_seq(crtc->scrn, dropped);
    }

    content = &queue->content[queue->count++];
    *content = (drmmode_prime_content_rec) {
        .seq = seq,
        .fb_id = fb_id,
        .buf_idx = buf_idx,
        .fence_fd = fence_fd,
        .reads_client = prime_scanout->direct.source != NULL,
        .compose_start = commit->compose_start,
    };
    if (buf_idx >= 0)
        prime_scanout->back_idx = (buf_idx + 1) %
            ARRAY_SIZE(prime_scanout->buf);
    if (!ms_prime_fence_signaled(fence_fd))
        content->watching_fence =
            SetNotifyFd(fence_fd, ms_prime_commit_fence_signaled,
                        X_NOTIFY_READ, crtc);
    ms_prime_tick_early(crtc, content);
    ms_prime_commit_schedule(crtc, 0);
}

static uint64_t
ms_prime_vblank_after(xf86CrtcPtr crtc, uint64_t when)
{
    uint64_t period = ms_prime_frame_period(crtc), ust, msc;

    if (!period || ms_get_crtc_ust_msc(crtc, &ust, &msc) != Success || !ust)
        return 0;
    if (when >= ust)
        return ust + ((when - ust) / period + 1) * period;
    return ust - (ust - when) / period * period;
}

#define MS_PRIME_TRIPLE_HOLD_USEC 10000000

/* A frame is late when its copy finished after the deadline of the vblank
 * following the start of its composition, which it would have missed without
 * working a frame ahead.
 */
static void
ms_prime_triple_update(xf86CrtcPtr crtc,
                       const drmmode_prime_content_rec *content)
{
    drmmode_prime_triple_rec *triple = &ms_prime_scanout(crtc)->commit.triple;
    uint64_t now = GetTimeInMicros(), done, vblank;

    if (content->buf_idx < 0 || !content->compose_start ||
        !ms_fence_signal_time(content->fence_fd, &done))
        return;

    vblank = ms_prime_vblank_after(crtc, content->compose_start);
    if (vblank && done + ms_prime_commit_margin(crtc) > vblank) {
        triple->active = TRUE;
        triple->last_late = now;
    } else if (triple->active && now - triple->last_late >
               MS_PRIME_TRIPLE_HOLD_USEC) {
        triple->active = FALSE;
    }
}

static void
ms_prime_flip_retry(xf86CrtcPtr crtc, uint32_t seq)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    ms_drm_abort_seq(crtc->scrn, seq);
    msGetPixmapPriv(drmmode_crtc->drmmode,
                    drmmode_crtc->prime_pixmap_back)->wait_for_damage = TRUE;
    ms_prime_kick(crtc);
}

static void
ms_prime_commit_failed(xf86CrtcPtr crtc, uint32_t seq, int error)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
               "PRIME flip on CRTC %u failed: %s\n",
               drmmode_crtc->mode_crtc->crtc_id, strerror(-error));
    ms_overlay_flip_failed(crtc);
    ms_prime_flip_retry(crtc, seq);
}

static void
ms_prime_flight_start(xf86CrtcPtr crtc, uint32_t seq, int buf_idx,
                      Bool cursor, const drmmode_prime_deadline_rec *deadline,
                      uint64_t start, uint64_t returned)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_prime_flight_rec *flight = &prime_scanout->commit.flight;
    drmmode_overlay_plane_rec *overlay = &drmmode_crtc->overlay_plane;

    *flight = (drmmode_prime_flight_rec) {
        .seq = seq,
        .buf_idx = buf_idx,
        .cursor = cursor,
    };
    if (!cursor) {
        flight->fb = prime_scanout->direct_fbs.pending;
        prime_scanout->direct_fbs.pending = (drmmode_prime_fb_rec) { 0 };
        flight->overlay = overlay->fb_id || overlay->buffers.pending;
    }
    prime_scanout->commit.last_target = deadline->vblank;
    ms_vblank_emul_committed(crtc, seq, start, returned, deadline->vblank);
}

static int
ms_prime_commit_content(xf86CrtcPtr crtc,
                        const drmmode_prime_deadline_rec *deadline)
{
    drmmode_prime_content_rec *head = ms_prime_queue_head(crtc);
    drmmode_prime_content_rec content = *head;
    uint64_t start, returned;
    int ret;

    ms_prime_content_unwatch(head);
    start = GetTimeInMicros();
    ret = drmmode_crtc_flip(crtc, content.fb_id, 0, 0,
                            DRM_MODE_PAGE_FLIP_EVENT, content.fence_fd,
                            (void *) (uintptr_t) content.seq);
    returned = GetTimeInMicros();
    if (ret == -EBUSY)
        return ret;

    if (ret == 0)
        ms_prime_triple_update(crtc, head);
    ms_prime_queue_drop(crtc, 0);
    if (ret) {
        ms_prime_commit_failed(crtc, content.seq, ret);
        return ret;
    }

    ms_prime_flight_start(crtc, content.seq, content.buf_idx, FALSE, deadline,
                          start, returned);
    if (!content.ticked)
        ms_prime_tick_start(crtc, content.seq, deadline);
    return 0;
}

static uint32_t
ms_prime_front_fb(xf86CrtcPtr crtc)
{
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);

    if (prime_scanout->direct_fbs.front.fb_id)
        return prime_scanout->direct_fbs.front.fb_id;
    return drmmode_prime_front_buffer(prime_scanout)->fb_id;
}

static void
ms_prime_cursor_flip_finished(void *data)
{
    xf86CrtcPtr crtc = data;
    drmmode_prime_flight_rec *flight = &ms_prime_scanout(crtc)->commit.flight;

    if (flight->cursor)
        *flight = (drmmode_prime_flight_rec) { .buf_idx = -1 };
}

static void
ms_prime_cursor_flip_done(uint64_t msc, uint64_t usec, void *data)
{
    xf86CrtcPtr crtc = data;

    ms_prime_cursor_flip_finished(crtc);
    ms_prime_commit_schedule(crtc, 0);
}

/* nvidia-drm only reports vblanks through flip events, so the cursor is
 * committed with a flip of the frame on screen, which keeps the vblank
 * estimate current while nothing else changes.
 */
static int
ms_prime_commit_cursor(xf86CrtcPtr crtc,
                       const drmmode_prime_deadline_rec *deadline)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    uint32_t fb_id = ms_prime_front_fb(crtc);
    uint64_t start, returned;
    uint32_t seq;
    int ret;

    if (!fb_id || drmmode_crtc->overlay_plane.next_fb_id ||
        drmmode_crtc_has_pending_hdr(crtc))
        return -EBUSY;

    seq = ms_drm_queue_alloc(crtc, crtc, ms_prime_cursor_flip_done,
                             ms_prime_cursor_flip_finished);
    if (!seq)
        return -ENOMEM;

    start = GetTimeInMicros();
    ret = drmmode_crtc_flip(crtc, fb_id, 0, 0, DRM_MODE_PAGE_FLIP_EVENT, -1,
                            (void *) (uintptr_t) seq);
    returned = GetTimeInMicros();
    if (ret)
        ms_drm_abort_seq(crtc->scrn, seq);
    else
        ms_prime_flight_start(crtc, seq, -1, TRUE, deadline, start, returned);
    return ret;
}

static int
ms_prime_commit_ready(xf86CrtcPtr crtc,
                      const drmmode_prime_deadline_rec *deadline)
{
    drmmode_prime_content_rec *head = ms_prime_queue_head(crtc);

    if (head && ms_prime_fence_signaled(head->fence_fd))
        return ms_prime_commit_content(crtc, deadline);
    if (ms_prime_cursor_wanted(crtc))
        return ms_prime_commit_cursor(crtc, deadline);
    return -EAGAIN;
}

static void
ms_prime_commit_now(xf86CrtcPtr crtc, int64_t late)
{
    drmmode_prime_commit_rec *commit = &ms_prime_scanout(crtc)->commit;
    drmmode_prime_deadline_rec deadline = commit->deadline;
    uint64_t start = GetTimeInMicros(), now;
    int ret;

    if (deadline.vblank &&
        start + ms_crtc_vblank_usec(crtc) >= deadline.vblank)
        ret = -ETIME;
    else
        ret = ms_prime_commit_ready(crtc, &deadline);
    now = GetTimeInMicros();

    if (ret == 0) {
        if (deadline.on_time && late <= MS_PRIME_COMMIT_LATE_CALLBACK_USEC &&
            now - start <= MS_PRIME_COMMIT_STALLED_USEC)
            ms_prime_commit_adapt(crtc, deadline.vblank, now);
        return;
    }

    ms_prime_commit_schedule(crtc, deadline.vblank ? deadline.vblank :
                             now + MS_PRIME_COMMIT_RETRY_USEC);
}

static void
ms_prime_deadline_fired(int fd, int ready, void *data)
{
    xf86CrtcPtr crtc = data;
    drmmode_prime_deadline_rec *deadline =
        &ms_prime_scanout(crtc)->commit.deadline;
    uint64_t at = deadline->at;

    if (!ms_prime_timer_expired(fd))
        return;

    deadline->at = 0;
    if (ms_prime_commit_wanted(crtc))
        ms_prime_commit_now(crtc, (int64_t) GetTimeInMicros() - (int64_t) at);
}

void
ms_prime_commit_pending(xf86CrtcPtr crtc)
{
    if (!ms_prime_scanout(crtc)->commit.deadline.at)
        ms_prime_commit_schedule(crtc, 0);
}

static void
ms_prime_frame_tick(xf86CrtcPtr crtc, uint64_t msc, uint64_t ust)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    uint32_t *tick_seq = &prime_scanout->commit.tick_seq;
    drmmode_prime_presenters_rec *presenters = &prime_scanout->presenters;

    if (!*tick_seq)
        return;

    if (drmmode_crtc->tearfree.flip_seq == *tick_seq)
        drmmode_crtc->tearfree.flip_seq = 0;
    *tick_seq = 0;

    *presenters = (drmmode_prime_presenters_rec) {
        .until = GetTimeInMicros() + MS_PRIME_PRESENTERS_WAIT_USEC,
    };
    presenters->count += ms_tearfree_dri_flipped(crtc, msc, ust);

    msGetPixmapPriv(drmmode_crtc->drmmode,
                    drmmode_crtc->prime_pixmap_back)->wait_for_damage = TRUE;
    ms_prime_kick(crtc);
}

/* A frame copied into a scanout buffer of our own is done with everything it
 * read once it is committed, and the third buffer keeps the next copy away
 * from it, so its clients can go on as soon as it is committed instead of
 * when nvidia reports the flip. Client buffers that are scanned out directly
 * and the overlay plane have no spare buffer, so they wait for the flip event.
 */
static void
ms_prime_tick_start(xf86CrtcPtr crtc, uint32_t seq,
                    const drmmode_prime_deadline_rec *deadline)
{
    drmmode_prime_commit_rec *commit = &ms_prime_scanout(crtc)->commit;

    commit->tick_seq = seq;
    if (deadline->vblank && commit->flight.buf_idx >= 0 &&
        !commit->flight.overlay)
        ms_prime_frame_tick(crtc, deadline->msc, deadline->vblank);
}

void
ms_prime_presented(xf86CrtcPtr crtc)
{
    ms_prime_scanout(crtc)->presenters.count--;
}

static Bool
ms_prime_flip_source_ready(PixmapPtr ppix, xf86CrtcPtr crtc, uint32_t seq)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_prime_direct_rec *direct = &prime_scanout->direct;
    PixmapPtr src = direct->source ? direct->source : ppix;
    drmmode_prime_fb_rec fb = { 0 };
    uint32_t fb_id;
    int buf_idx = -1;
    int fence_fd = -1;

    if (direct->scanout_source &&
        ms_import_flip_fb(screen, src, &fb.fb_id, "PRIME direct-scanout")) {
        fb.pixmap = src;
        src->refcnt++;
        fb_id = fb.fb_id;
    } else {
        ms_prime_release_fb(crtc, &fb);
        buf_idx = prime_scanout->back_idx;
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

    prime_scanout->direct_fbs.pending = fb;
    drmmode_crtc->tearfree.flip_seq = seq;
    ms_prime_commit_queue(crtc, seq, fb_id, buf_idx, fence_fd);
    return TRUE;
}

static void
ms_prime_source_wait_cancel(xf86CrtcPtr crtc)
{
    drmmode_prime_source_wait_rec *wait = &ms_prime_scanout(crtc)->source_wait;

    if (!wait->seq)
        return;

    SetNotifyFd(wait->fence_fd, NULL, 0, NULL);
    close(wait->fence_fd);
    wait->seq = 0;
}

static void
ms_prime_source_signaled(int fd, int ready, void *data)
{
    xf86CrtcPtr crtc = data;
    drmmode_prime_source_wait_rec wait = ms_prime_scanout(crtc)->source_wait;

    ms_prime_source_wait_cancel(crtc);
    if (!ms_prime_flip_source_ready(wait.pixmap, crtc, wait.seq))
        ms_prime_flip_retry(crtc, wait.seq);
}

static int
ms_prime_source_fence(xf86CrtcPtr crtc, PixmapPtr ppix)
{
    drmmode_prime_direct_rec *direct = &ms_prime_scanout(crtc)->direct;
    msPixmapPrivPtr ppriv;
    ScrnInfoPtr scrn;
    modesettingPtr ms;

    if (!direct->source) {
        ppriv = ms_prime_source_priv(ppix);
        return ppriv ? ms_dmabuf_writer_fence(ppriv->dmabuf_fd) : -1;
    }
    if (direct->scanout_source || !direct->pixmap)
        return -1;

    scrn = xf86ScreenToScrn(direct->pixmap->drawable.pScreen);
    if (!ms_scrn_is_modesetting(scrn))
        return -1;

    ms = modesettingPTR(scrn);
    return ms->glamor.egl_pixmap_rendering_fd ?
        ms->glamor.egl_pixmap_rendering_fd(direct->pixmap) : -1;
}

/* An nvidia copy from a buffer another GPU is still rendering finishes about
 * 7 ms after that rendering, so the copy is submitted once it is done.
 */
Bool
ms_prime_flip(PixmapPtr ppix, xf86CrtcPtr crtc, uint32_t seq)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    int fence_fd;

    if (prime_scanout->direct.covered)
        ms_prime_overlay_prepare(crtc);
    ms_prime_discard_front_damage(crtc);

    fence_fd = ms_prime_source_fence(crtc, ppix);
    if (fence_fd < 0)
        return ms_prime_flip_source_ready(ppix, crtc, seq);

    prime_scanout->source_wait = (drmmode_prime_source_wait_rec) {
        .pixmap = ppix,
        .seq = seq,
        .fence_fd = fence_fd,
    };
    drmmode_crtc->tearfree.flip_seq = seq;
    SetNotifyFd(fence_fd, ms_prime_source_signaled, X_NOTIFY_READ, crtc);
    return TRUE;
}

void
ms_prime_flip_done(xf86CrtcPtr crtc, uint32_t seq, uint64_t msc,
                   uint64_t usec)
{
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_prime_commit_rec *commit = &prime_scanout->commit;
    drmmode_prime_flight_rec *flight = &commit->flight;

    if (seq == flight->seq) {
        if (flight->buf_idx >= 0)
            prime_scanout->front_idx = flight->buf_idx;
        ms_prime_release_fb(crtc, &prime_scanout->direct_fbs.front);
        prime_scanout->direct_fbs.front = flight->fb;
        if (flight->overlay)
            ms_overlay_flip_done(crtc);
        *flight = (drmmode_prime_flight_rec) { .buf_idx = -1 };
    }

    if (seq == commit->tick_seq)
        ms_prime_frame_tick(crtc, msc, usec);
    ms_prime_commit_schedule(crtc, 0);
}

void
ms_prime_flip_aborted(xf86CrtcPtr crtc, uint32_t seq)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_prime_commit_rec *commit = &prime_scanout->commit;
    Bool matched = FALSE;

    if (seq == commit->flight.seq) {
        ms_prime_release_fb(crtc, &commit->flight.fb);
        if (commit->flight.overlay)
            ms_overlay_flip_aborted(crtc);
        commit->flight = (drmmode_prime_flight_rec) { .buf_idx = -1 };
        matched = TRUE;
    }

    if (seq == commit->tick_seq) {
        commit->tick_seq = 0;
        matched = TRUE;
    }

    if (ms_prime_queue_remove(crtc, seq))
        matched = TRUE;

    if (seq == drmmode_crtc->tearfree.flip_seq) {
        ms_prime_source_wait_cancel(crtc);
        ms_prime_release_fb(crtc, &prime_scanout->direct_fbs.pending);
        ms_overlay_flip_aborted(crtc);
        drmmode_crtc->tearfree.flip_seq = 0;
        matched = TRUE;
    }

    if (matched)
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
    msPixmapPrivPtr ppriv = msGetPixmapPriv(drmmode_crtc->drmmode,
                                            drmmode_crtc->prime_pixmap_back);

    ms_prime_scanout(crtc)->commit.compose_start = GetTimeInMicros();
    ppriv->wait_for_damage = FALSE;
    if (!drmmode_SharedPixmapPresentNow(drmmode_crtc->prime_pixmap_back, crtc,
                                        drmmode_crtc->drmmode))
        ppriv->wait_for_damage = TRUE;
}

/* The shared pixmap is painted again for the next frame, so the copy of the
 * frame queued last must be done, and the copy needs a buffer that is neither
 * on screen nor committed or queued.
 */
static Bool
ms_prime_can_compose(xf86CrtcPtr crtc)
{
    drmmode_prime_scanout_rec *prime_scanout = ms_prime_scanout(crtc);
    drmmode_prime_commit_rec *commit = &prime_scanout->commit;
    drmmode_prime_content_rec *tail = ms_prime_queue_tail(crtc);
    int back = prime_scanout->back_idx, i;

    if (tail && !ms_prime_fence_signaled(tail->fence_fd))
        return FALSE;

    if (back == prime_scanout->front_idx ||
        (commit->flight.seq && commit->flight.buf_idx == back))
        return FALSE;

    for (i = 0; i < commit->queue.count; i++) {
        if (commit->queue.content[i].buf_idx == back)
            return FALSE;
    }
    return TRUE;
}

/* Clients whose frame was just completed usually present the next one right
 * away, so composition waits a moment for them instead of leaving them out.
 */
static Bool
ms_prime_compose_ready(xf86CrtcPtr crtc, void *timeout)
{
    drmmode_prime_presenters_rec *presenters =
        &ms_prime_scanout(crtc)->presenters;
    uint64_t now = GetTimeInMicros();

    if (presenters->count <= 0 || now >= presenters->until)
        return TRUE;

    AdjustWaitForDelay(timeout, (presenters->until - now + 999) / 1000);
    return FALSE;
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

        if (!ms_prime_can_compose(crtc) ||
            !ms_prime_compose_ready(crtc, timeout))
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
