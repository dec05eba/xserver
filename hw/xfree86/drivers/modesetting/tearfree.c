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
#include <xf86drm.h>
#include "driver.h"

#ifdef HAVE_GBM

#define MS_TEARFREE_COMPOSE_SLACK_USEC 2000
#define MS_TEARFREE_LATE_USEC 1500
#define MS_COMPOSE_HOLD_USEC 10000000
#define MS_COMPOSE_ESTIMATE_DECAY 16

static drmmode_tearfree_ptr
ms_tearfree(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return &drmmode_crtc->tearfree;
}

static drmmode_tearfree_frame_ptr
ms_tearfree_frame_at(drmmode_tearfree_ptr trf, int position)
{
    return &trf->frames.frame[trf->frames.order[position]];
}

static drmmode_tearfree_frame_ptr
ms_tearfree_oldest_frame(drmmode_tearfree_ptr trf)
{
    return trf->frames.count ? ms_tearfree_frame_at(trf, 0) : NULL;
}

static uint32_t
ms_tearfree_frame_buffer(drmmode_tearfree_ptr trf, drmmode_tearfree_frame_ptr frame)
{
    return frame - trf->frames.frame;
}

uint32_t
ms_tearfree_newest_buffer(drmmode_tearfree_ptr trf)
{
    return trf->frames.count ? trf->frames.order[trf->frames.count - 1] : trf->front_idx;
}

uint32_t
ms_tearfree_pending_seq(drmmode_tearfree_ptr trf)
{
    if (trf->frames.count)
        return ms_tearfree_frame_at(trf, trf->frames.count - 1)->seq;
    return trf->flip_seq;
}

Bool
ms_tearfree_seq_pending(drmmode_tearfree_ptr trf, uint32_t seq)
{
    int i;

    if (seq == trf->flip_seq)
        return TRUE;
    for (i = 0; i < trf->frames.count; i++) {
        if (ms_tearfree_frame_at(trf, i)->seq == seq)
            return TRUE;
    }
    return FALSE;
}

static int
ms_tearfree_free_buffer(drmmode_tearfree_ptr trf)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(trf->buf); i++) {
        if (i != trf->front_idx && !trf->frames.frame[i].seq)
            return i;
    }
    return -1;
}

static void
ms_compose_timing_done(drmmode_compose_timing_rec *compose, uint64_t usec)
{
    drmmode_compose_hold_rec *hold = &compose->hold;
    uint64_t now = GetTimeInMicros();

    if (usec > compose->usec + MS_TEARFREE_COMPOSE_SLACK_USEC) {
        hold->usec = now < hold->until ? max(hold->usec, usec) : usec;
        hold->until = now + MS_COMPOSE_HOLD_USEC;
    }
    compose->usec = max(compose->usec, usec);
}

static void
ms_compose_timing_measured(drmmode_compose_timing_rec *compose, uint64_t usec)
{
    compose->usec = max(usec, compose->usec -
                        compose->usec / MS_COMPOSE_ESTIMATE_DECAY);
}

static uint64_t
ms_compose_margin(const drmmode_compose_timing_rec *compose, uint64_t now)
{
    uint64_t usec = compose->usec;

    if (now < compose->hold.until)
        usec = max(usec, compose->hold.usec);
    return usec + MS_TEARFREE_COMPOSE_SLACK_USEC;
}

static void
ms_tearfree_frame_release(drmmode_tearfree_ptr trf, drmmode_tearfree_frame_ptr frame, Bool shown)
{
    drmmode_tearfree_frames_rec *frames = &trf->frames;
    uint32_t buffer = ms_tearfree_frame_buffer(trf, frame);
    uint64_t signaled;
    int i, kept = 0;

    if (frame->fence_fd >= 0) {
        SetNotifyFd(frame->fence_fd, NULL, 0, NULL);
        if (shown && ms_fence_signal_time(frame->fence_fd, &signaled))
            ms_compose_timing_done(&trf->compose, signaled > frame->start ? signaled - frame->start : 0);
        close(frame->fence_fd);
        frame->fence_fd = -1;
    }
    frame->seq = 0;
    frame->committed = FALSE;

    for (i = 0; i < frames->count; i++) {
        if (frames->order[i] != buffer)
            frames->order[kept++] = frames->order[i];
    }
    frames->count = kept;
}

static void
ms_tearfree_frame_aborted(void *data)
{
    drmmode_tearfree_frame_ptr frame = data;
    xf86CrtcPtr crtc = frame->crtc;
    drmmode_tearfree_ptr trf = ms_tearfree(crtc);
    Bool committed = frame->committed;

    if (committed)
        trf->flip_seq = 0;
    ms_tearfree_frame_release(trf, frame, FALSE);
    if (committed)
        ms_overlay_flip_aborted(crtc);
    ms_tearfree_dri_abort_all(crtc);
}

static void
ms_tearfree_frame_shown(uint64_t msc, uint64_t usec, void *data)
{
    drmmode_tearfree_frame_ptr frame = data;
    xf86CrtcPtr crtc = frame->crtc;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_tearfree_ptr trf = &drmmode_crtc->tearfree;
    uint32_t old_fb_id = trf->scanout.fb_id, seq = frame->seq;

    trf->front_idx = ms_tearfree_frame_buffer(trf, frame);
    trf->scanout.fb_id = 0;
    trf->flip_seq = 0;
    ms_tearfree_frame_release(trf, frame, TRUE);
    ms_overlay_flip_done(crtc);

    if (old_fb_id)
        drmModeRmFB(drmmode_crtc->drmmode->fd, old_fb_id);

    ms_tearfree_dri_notify(trf, seq, msc, usec);
}

static Bool
ms_tearfree_frame_finished(drmmode_tearfree_frame_ptr frame)
{
    return frame->fence_fd < 0 || ms_fence_signaled(frame->fence_fd);
}

static void
ms_tearfree_frame_composed(int fd, int xevents, void *data)
{
    SetNotifyFd(fd, NULL, 0, NULL);
    SetBlockHandlerDeadline(GetTimeInMicros());
}

Bool
ms_tearfree_next_vblank(xf86CrtcPtr crtc, uint64_t now, uint64_t *next,
                        uint64_t *next_msc)
{
    double refresh = xf86ModeVRefresh(&crtc->mode);
    uint64_t ust, msc, period, elapsed, frames;

    if (refresh <= 0.0 || ms_get_crtc_ust_msc(crtc, &ust, &msc) != Success ||
        !ust)
        return FALSE;

    /* The vblank timestamp is the end of vblank, so it's slightly in the future
     * while the vblank is being handled.
     */
    period = 1000000.0 / refresh;
    elapsed = now > ust ? now - ust : 0;
    frames = elapsed / period + 1;
    *next = ust + frames * period;
    if (next_msc)
        *next_msc = msc + frames;
    return TRUE;
}

Bool
ms_tearfree_flip_wanted(xf86CrtcPtr crtc)
{
    drmmode_tearfree_ptr trf = ms_tearfree(crtc);

    return RegionNotEmpty(&trf->buf[ms_tearfree_newest_buffer(trf)].dmg) ||
        trf->scanout.fb_id || drmmode_crtc_has_pending_hdr(crtc) ||
        ms_tearfree_dri_waiting(trf);
}

Bool
ms_tearfree_compose_due(xf86CrtcPtr crtc, void *timeout)
{
    uint64_t now = GetTimeInMicros();
    uint64_t margin = ms_compose_margin(&ms_tearfree(crtc)->compose, now);
    uint64_t next;

    if (!ms_tearfree_next_vblank(crtc, now, &next, NULL) ||
        next - now <= margin)
        return TRUE;

    AdjustWaitForDelay(timeout, (next - margin - now + 999) / 1000);
    return FALSE;
}

Bool
ms_tearfree_composition_late(xf86CrtcPtr crtc)
{
    uint64_t now = GetTimeInMicros();
    uint64_t next;

    return !ms_tearfree_next_vblank(crtc, now, &next, NULL) ||
        next - now <= MS_TEARFREE_LATE_USEC;
}

Bool
ms_tearfree_compose_ahead(xf86CrtcPtr crtc)
{
    drmmode_tearfree_ptr trf = ms_tearfree(crtc);
    uint64_t next, next_msc;

    return trf->frames.count == 1 &&
        ms_tearfree_next_vblank(crtc, GetTimeInMicros(), &next, &next_msc) &&
        next_msc > ms_tearfree_oldest_frame(trf)->target_msc;
}

void
ms_tearfree_set_compose_deadline(xf86CrtcPtr crtc)
{
    drmmode_tearfree_ptr trf = ms_tearfree(crtc);
    drmmode_tearfree_frame_ptr oldest = ms_tearfree_oldest_frame(trf);
    uint64_t now = GetTimeInMicros();
    uint64_t margin = ms_compose_margin(&trf->compose, now);
    uint64_t next;

    if (ms_tearfree_next_vblank(crtc, now + margin, &next, NULL))
        SetBlockHandlerDeadline(next - margin);

    if (oldest && !oldest->committed &&
        ms_tearfree_next_vblank(crtc, now + MS_TEARFREE_LATE_USEC, &next, NULL))
        SetBlockHandlerDeadline(next - MS_TEARFREE_LATE_USEC);
}

static void
ms_tearfree_compose(xf86CrtcPtr crtc, drmmode_tearfree_frame_ptr frame)
{
    drmmode_tearfree_ptr trf = ms_tearfree(crtc);
    drmmode_shadow_fb_ptr buf = &trf->buf[ms_tearfree_frame_buffer(trf, frame)];

    frame->start = GetTimeInMicros();
    frame->fence_fd = drmmode_compose_damage_fenced(crtc, buf->px, &buf->dmg);
    ms_compose_timing_measured(&trf->compose, GetTimeInMicros() - frame->start);
}

static void
ms_tearfree_abort_uncommitted(xf86CrtcPtr crtc)
{
    drmmode_tearfree_ptr trf = ms_tearfree(crtc);
    int i;

    for (i = trf->frames.count - 1; i >= 0; i--) {
        drmmode_tearfree_frame_ptr frame = ms_tearfree_frame_at(trf, i);

        if (!frame->committed)
            ms_drm_abort_seq(crtc->scrn, frame->seq);
    }
}

static Bool
ms_tearfree_flip_failed(xf86CrtcPtr crtc)
{
    drmmode_tearfree_ptr trf = ms_tearfree(crtc);
    drmmode_shadow_fb_ptr front = &trf->buf[trf->front_idx];

    xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
               "TearFree flip failed, rendering frame without TearFree\n");
    ms_tearfree_abort_uncommitted(crtc);
    drmmode_compose_damage(crtc, front->px, &front->dmg, FALSE);
    return TRUE;
}

Bool
ms_tearfree_commit_composed(ScreenPtr screen, xf86CrtcPtr crtc)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_tearfree_ptr trf = ms_tearfree(crtc);
    drmmode_tearfree_frame_ptr oldest = ms_tearfree_oldest_frame(trf);
    Bool failed;

    if (!oldest || oldest->committed || trf->flip_seq || ms_cursor_in_flight(crtc) ||
        ms->drmmode.dri2_flipping || ms->drmmode.present_flipping ||
        !ms_tearfree_frame_finished(oldest) ||
        !ms_overlay_owner_ready(crtc, oldest->overlay_owner))
        return FALSE;

    ms_overlay_begin_commit(crtc, oldest->overlay_owner);
    failed = do_queue_flip_on_crtc(screen, crtc, DRM_MODE_PAGE_FLIP_EVENT, oldest->seq,
                                   trf->buf[ms_tearfree_frame_buffer(trf, oldest)].fb_id, 0, 0, -1);
    ms_overlay_end_commit(crtc, !failed, oldest->seq);
    if (failed)
        return ms_tearfree_flip_failed(crtc);

    oldest->committed = TRUE;
    trf->flip_seq = oldest->seq;
    trf->scanout.window = NULL;
    trf->scanout.pixmap = NULL;
    return FALSE;
}

Bool
ms_do_tearfree_flip(ScreenPtr screen, xf86CrtcPtr crtc)
{
    drmmode_tearfree_ptr trf = ms_tearfree(crtc);
    drmmode_tearfree_frames_rec *frames = &trf->frames;
    int buffer = ms_tearfree_free_buffer(trf);
    drmmode_tearfree_frame_ptr frame;
    uint64_t next;

    if (buffer < 0 || frames->count == ARRAY_SIZE(frames->order))
        return FALSE;

    ms_compositor_update_composed_output_mode(crtc);
    frame = &frames->frame[buffer];
    frame->seq = ms_drm_queue_alloc(crtc, frame, ms_tearfree_frame_shown,
                                    ms_tearfree_frame_aborted);
    if (!frame->seq) {
        /* Need to notify the DRI clients if a sequence wasn't allocated. Once a
         * sequence is allocated, explicitly performing this cleanup isn't
         * necessary since it's already done as part of aborting the sequence.
         */
        ms_tearfree_dri_abort_all(crtc);
        return ms_tearfree_flip_failed(crtc);
    }

    frame->crtc = crtc;
    frame->committed = FALSE;
    frame->overlay_owner = ms_overlay_frame_owner(crtc);
    if (!ms_tearfree_next_vblank(crtc, GetTimeInMicros(), &next, &frame->target_msc))
        frame->target_msc = 0;
    frames->order[frames->count++] = buffer;

    ms_tearfree_compose(crtc, frame);
    ms_tearfree_dri_latch(trf);
    if (!ms_tearfree_frame_finished(frame))
        SetNotifyFd(frame->fence_fd, ms_tearfree_frame_composed, X_NOTIFY_READ, NULL);
    ms_tearfree_set_compose_deadline(crtc);

    return ms_tearfree_commit_composed(screen, crtc);
}

#endif
