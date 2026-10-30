/*
 * Copyright © 2013 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

/** @file vblank.c
 *
 * Support for tracking the DRM's vblank events.
 */

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <errno.h>
#include <unistd.h>

#include <xf86.h>
#include <xf86Crtc.h>
#include "driver.h"
#include "drmmode_display.h"

/**
 * Tracking for outstanding events queued to the kernel.
 *
 * Each list entry is a struct ms_drm_queue, which has a uint32_t
 * value generated from drm_seq that identifies the event and a
 * reference back to the crtc/screen associated with the event.  It's
 * done this way rather than in the screen because we want to be able
 * to drain the list of event handlers that should be called at server
 * regen time, even though we don't close the drm fd and have no way
 * to actually drain the kernel events.
 */
static struct xorg_list ms_drm_queue;
static uint32_t ms_drm_seq;

static void box_intersect(BoxPtr dest, BoxPtr a, BoxPtr b)
{
    dest->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    dest->x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    if (dest->x1 >= dest->x2) {
        dest->x1 = dest->x2 = dest->y1 = dest->y2 = 0;
        return;
    }

    dest->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    dest->y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    if (dest->y1 >= dest->y2)
        dest->x1 = dest->x2 = dest->y1 = dest->y2 = 0;
}

static void rr_crtc_box(RRCrtcPtr crtc, BoxPtr crtc_box)
{
    if (crtc->mode) {
        crtc_box->x1 = crtc->x;
        crtc_box->y1 = crtc->y;
        switch (crtc->rotation) {
            case RR_Rotate_0:
            case RR_Rotate_180:
            default:
                crtc_box->x2 = crtc->x + crtc->mode->mode.width;
                crtc_box->y2 = crtc->y + crtc->mode->mode.height;
                break;
            case RR_Rotate_90:
            case RR_Rotate_270:
                crtc_box->x2 = crtc->x + crtc->mode->mode.height;
                crtc_box->y2 = crtc->y + crtc->mode->mode.width;
                break;
        }
    } else
        crtc_box->x1 = crtc_box->x2 = crtc_box->y1 = crtc_box->y2 = 0;
}

static int box_area(BoxPtr box)
{
    return (int)(box->x2 - box->x1) * (int)(box->y2 - box->y1);
}

static Bool rr_crtc_on(RRCrtcPtr crtc, Bool crtc_is_xf86_hint)
{
    if (!crtc) {
        return FALSE;
    }
    if (crtc_is_xf86_hint && crtc->devPrivate) {
         return xf86_crtc_on(crtc->devPrivate);
    } else {
        return !!crtc->mode;
    }
}

Bool
xf86_crtc_on(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return crtc->enabled && drmmode_crtc->dpms_mode == DPMSModeOn;
}


/*
 * Return the crtc covering 'box'. If two crtcs cover a portion of
 * 'box', then prefer the crtc with greater coverage.
 */
static RRCrtcPtr
rr_crtc_covering_box(ScreenPtr pScreen, BoxPtr box, Bool screen_is_xf86_hint,
                     int *coverage_out)
{
    rrScrPrivPtr pScrPriv;
    RROutputPtr primary_output;
    RRCrtcPtr crtc, best_crtc, primary_crtc;
    int coverage, best_coverage;
    int c;
    BoxRec crtc_box, cover_box;

    best_crtc = NULL;
    best_coverage = 0;
    if (coverage_out)
        *coverage_out = 0;

    if (!dixPrivateKeyRegistered(rrPrivKey))
        return NULL;

    pScrPriv = rrGetScrPriv(pScreen);

    if (!pScrPriv)
        return NULL;

    primary_crtc = NULL;
    primary_output = RRFirstOutput(pScreen);
    if (primary_output)
        primary_crtc = primary_output->crtc;

    for (c = 0; c < pScrPriv->numCrtcs; c++) {
        crtc = pScrPriv->crtcs[c];

        /* If the CRTC is off, treat it as not covering */
        if (!rr_crtc_on(crtc, screen_is_xf86_hint))
            continue;

        rr_crtc_box(crtc, &crtc_box);
        box_intersect(&cover_box, &crtc_box, box);
        coverage = box_area(&cover_box);
        if ((coverage > best_coverage) ||
            (coverage == best_coverage && crtc == primary_crtc)) {
            best_crtc = crtc;
            best_coverage = coverage;
        }
    }

    if (coverage_out)
        *coverage_out = best_coverage;
    return best_crtc;
}

static RRCrtcPtr
rr_crtc_covering_box_on_secondary(ScreenPtr pScreen, BoxPtr box,
                                  int *coverage_out)
{
    RRCrtcPtr best_crtc = NULL;
    int best_coverage = 0;

    if (!pScreen->isGPU) {
        ScreenPtr secondary;

        xorg_list_for_each_entry(secondary, &pScreen->secondary_list, secondary_head) {
            RRCrtcPtr crtc;
            int coverage;

            if (!secondary->is_output_secondary)
                continue;

            crtc = rr_crtc_covering_box(secondary, box, FALSE, &coverage);
            if (crtc && coverage > best_coverage) {
                best_crtc = crtc;
                best_coverage = coverage;
            }
        }
    }

    *coverage_out = best_coverage;
    return best_crtc;
}

xf86CrtcPtr
ms_dri2_crtc_covering_drawable(DrawablePtr pDraw)
{
    ScreenPtr pScreen = pDraw->pScreen;
    RRCrtcPtr crtc = NULL;
    BoxRec box;

    box.x1 = pDraw->x;
    box.y1 = pDraw->y;
    box.x2 = box.x1 + pDraw->width;
    box.y2 = box.y1 + pDraw->height;

    crtc = rr_crtc_covering_box(pScreen, &box, TRUE, NULL);
    if (crtc) {
        return crtc->devPrivate;
    }
    return NULL;
}

RRCrtcPtr
ms_randr_crtc_covering_drawable(DrawablePtr pDraw)
{
    ScreenPtr pScreen = pDraw->pScreen;
    RRCrtcPtr crtc, secondary_crtc;
    int coverage, secondary_coverage;
    BoxRec box;

    box.x1 = pDraw->x;
    box.y1 = pDraw->y;
    box.x2 = box.x1 + pDraw->width;
    box.y2 = box.y1 + pDraw->height;

    crtc = rr_crtc_covering_box(pScreen, &box, TRUE, &coverage);
    secondary_crtc = rr_crtc_covering_box_on_secondary(pScreen, &box,
                                                       &secondary_coverage);
    if (secondary_crtc && secondary_coverage > coverage)
        return secondary_crtc;
    return crtc;
}

static void
ms_drm_sequence_handler(int fd, uint64_t frame, uint64_t ns, Bool is64bit,
                        uint64_t user_data);
static void
ms_drm_set_seq_queued(uint32_t seq, uint64_t msc);

/*
 * Some kernel drivers, such as nvidia-drm, don't support vblank queries or
 * events, only page flip events. Emulate vblanks for them from the refresh
 * rate of the mode, aligned to the page flip completion timestamps.
 */
static void
ms_emulate_vblank(modesettingPtr ms, ScrnInfoPtr scrn)
{
    if (ms->emulate_vblank)
        return;

    ms->emulate_vblank = TRUE;
    xf86DrvMsg(scrn->scrnIndex, X_INFO,
               "Kernel doesn't support vblank events, emulating them\n");
}

static uint64_t
ms_crtc_frame_period(xf86CrtcPtr crtc)
{
    double refresh = xf86ModeVRefresh(&crtc->mode);

    return refresh > 0.0 ? 1000000.0 / refresh : 16667;
}

static void
ms_vblank_emul_at(xf86CrtcPtr crtc, uint64_t now, uint64_t *msc,
                  uint64_t *ust)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_vblank_emul_rec *emul = &drmmode_crtc->vblank_emul;
    uint64_t period = ms_crtc_frame_period(crtc);
    uint64_t frames;

    if (!emul->base_ust || emul->period != period) {
        if (emul->base_ust && now > emul->base_ust)
            emul->base_msc += (now - emul->base_ust) / emul->period;
        emul->base_ust = now;
        emul->period = period;
    }

    frames = now > emul->base_ust ? (now - emul->base_ust) / period : 0;
    *msc = emul->base_msc + frames;
    *ust = emul->base_ust + frames * period;
}

static uint64_t
ms_vblank_emul_flipped(xf86CrtcPtr crtc, uint64_t ust)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_vblank_emul_rec *emul = &drmmode_crtc->vblank_emul;
    uint64_t msc, vblank_ust;

    ms_vblank_emul_at(crtc, ust, &msc, &vblank_ust);
    if (ust > vblank_ust && ust - vblank_ust > emul->period / 2)
        msc++;

    emul->base_msc = msc;
    emul->base_ust = ust;
    return msc;
}

static CARD32
ms_vblank_emul_timeout(OsTimerPtr timer, CARD32 time, void *arg);

static void
ms_vblank_emul_arm(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_vblank_emul_rec *emul = &drmmode_crtc->vblank_emul;
    uint64_t target = UINT64_MAX, now = GetTimeInMicros(), msc, ust, when;
    struct ms_drm_queue *q;

    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (q->crtc == crtc && q->kernel_queued && q->msc < target)
            target = q->msc;
    }

    if (target == UINT64_MAX) {
        TimerCancel(emul->timer);
        return;
    }

    ms_vblank_emul_at(crtc, now, &msc, &ust);
    when = target > msc ? ust + (target - msc) * emul->period : now;
    emul->timer = TimerSet(emul->timer, 0,
                           max((when - min(when, now) + 999) / 1000, 1),
                           ms_vblank_emul_timeout, crtc);
}

static CARD32
ms_vblank_emul_timeout(OsTimerPtr timer, CARD32 time, void *arg)
{
    xf86CrtcPtr crtc = arg;
    struct ms_drm_queue *q;
    uint64_t msc, ust;

    ms_vblank_emul_at(crtc, GetTimeInMicros(), &msc, &ust);
    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (q->crtc == crtc && q->kernel_queued && q->msc <= msc) {
            ms_drm_sequence_handler(-1, msc, ust * 1000, TRUE, q->seq);
            break;
        }
    }

    ms_vblank_emul_arm(crtc);
    return 0;
}

static Bool
ms_vblank_emul_queue(xf86CrtcPtr crtc, ms_queue_flag flags, uint64_t msc,
                     uint64_t *msc_queued, uint32_t seq)
{
    uint64_t now_msc, ust;

    if (!xf86_crtc_on(crtc)) {
        ms_drm_abort_seq(crtc->scrn, seq);
        return FALSE;
    }

    ms_vblank_emul_at(crtc, GetTimeInMicros(), &now_msc, &ust);
    if (flags & MS_QUEUE_RELATIVE)
        msc += now_msc;
    else if ((flags & MS_QUEUE_NEXT_ON_MISS) && msc <= now_msc)
        msc = now_msc + 1;

    ms_drm_set_seq_queued(seq, msc);
    if (msc_queued)
        *msc_queued = msc;
    ms_vblank_emul_arm(crtc);
    return TRUE;
}

void
ms_vblank_crtc_fini(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    TimerFree(drmmode_crtc->vblank_emul.timer);
    drmmode_crtc->vblank_emul.timer = NULL;
}

static Bool
ms_get_kernel_ust_msc(xf86CrtcPtr crtc,
                      uint64_t *msc, uint64_t *ust)
{
    ScreenPtr screen = crtc->randr_crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmVBlank vbl;
    int ret;

    if (ms->emulate_vblank) {
        if (!xf86_crtc_on(crtc))
            return FALSE;
        ms_vblank_emul_at(crtc, GetTimeInMicros(), msc, ust);
        return TRUE;
    }

    if (ms->has_queue_sequence || !ms->tried_queue_sequence) {
        uint64_t ns;
        ms->tried_queue_sequence = TRUE;

        ret = drmCrtcGetSequence(ms->fd, drmmode_crtc->mode_crtc->crtc_id,
                                 msc, &ns);
        if (ret == -1 && errno == EOPNOTSUPP) {
            ms_emulate_vblank(ms, scrn);
            return ms_get_kernel_ust_msc(crtc, msc, ust);
        }
        if (ret != -1 || (errno != ENOTTY && errno != EINVAL)) {
            ms->has_queue_sequence = TRUE;
            if (ret == 0)
                *ust = ns / 1000;
            return ret == 0;
        }
    }
    /* Get current count */
    vbl.request.type = DRM_VBLANK_RELATIVE | drmmode_crtc->vblank_pipe;
    vbl.request.sequence = 0;
    vbl.request.signal = 0;
    ret = drmWaitVBlank(ms->fd, &vbl);
    if (ret && errno == EOPNOTSUPP) {
        ms_emulate_vblank(ms, scrn);
        return ms_get_kernel_ust_msc(crtc, msc, ust);
    }
    if (ret) {
        *msc = 0;
        *ust = 0;
        return FALSE;
    } else {
        *msc = vbl.reply.sequence;
        *ust = (CARD64) vbl.reply.tval_sec * 1000000 + vbl.reply.tval_usec;
        return TRUE;
    }
}

static void
ms_drm_set_seq_msc(uint32_t seq, uint64_t msc)
{
    struct ms_drm_queue *q;

    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (q->seq == seq) {
            q->msc = msc;
            break;
        }
    }
}

static void
ms_drm_set_seq_queued(uint32_t seq, uint64_t msc)
{
    drmmode_crtc_private_ptr drmmode_crtc;
    struct ms_drm_queue *q;

    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (q->seq == seq) {
            drmmode_crtc = q->crtc->driver_private;
            if (msc < drmmode_crtc->next_msc)
                drmmode_crtc->next_msc = msc;
            q->msc = msc;
            q->kernel_queued = TRUE;
            break;
        }
    }
}

static Bool
ms_queue_coalesce(xf86CrtcPtr crtc, uint32_t seq, uint64_t msc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    /* If the next MSC is too late, then this event can't be coalesced */
    if (msc < drmmode_crtc->next_msc)
        return FALSE;

    /* Set the target MSC on this sequence number */
    ms_drm_set_seq_msc(seq, msc);
    return TRUE;
}

Bool
ms_queue_vblank(xf86CrtcPtr crtc, ms_queue_flag flags,
                uint64_t msc, uint64_t *msc_queued, uint32_t seq)
{
    ScreenPtr screen = crtc->randr_crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmVBlank vbl;
    int ret;

    /* Try coalescing this event into another to avoid event queue exhaustion */
    if (flags == MS_QUEUE_ABSOLUTE && ms_queue_coalesce(crtc, seq, msc))
        return TRUE;

    for (;;) {
        if (ms->emulate_vblank)
            return ms_vblank_emul_queue(crtc, flags, msc, msc_queued, seq);

        /* Queue an event at the specified sequence */
        if (ms->has_queue_sequence || !ms->tried_queue_sequence) {
            uint32_t drm_flags = 0;
            uint64_t kernel_queued;

            ms->tried_queue_sequence = TRUE;

            if (flags & MS_QUEUE_RELATIVE)
                drm_flags |= DRM_CRTC_SEQUENCE_RELATIVE;
            if (flags & MS_QUEUE_NEXT_ON_MISS)
                drm_flags |= DRM_CRTC_SEQUENCE_NEXT_ON_MISS;

            ret = drmCrtcQueueSequence(ms->fd, drmmode_crtc->mode_crtc->crtc_id,
                                       drm_flags, msc, &kernel_queued, seq);
            if (ret == 0) {
                msc = ms_kernel_msc_to_crtc_msc(crtc, kernel_queued, TRUE);
                ms_drm_set_seq_queued(seq, msc);
                if (msc_queued)
                    *msc_queued = msc;
                ms->has_queue_sequence = TRUE;
                return TRUE;
            }

            if (ret == -1 && errno == EOPNOTSUPP) {
                ms_emulate_vblank(ms, scrn);
                continue;
            }
            if (ret != -1 || (errno != ENOTTY && errno != EINVAL)) {
                ms->has_queue_sequence = TRUE;
                goto check;
            }
        }
        vbl.request.type = DRM_VBLANK_EVENT | drmmode_crtc->vblank_pipe;
        if (flags & MS_QUEUE_RELATIVE)
            vbl.request.type |= DRM_VBLANK_RELATIVE;
        else
            vbl.request.type |= DRM_VBLANK_ABSOLUTE;
        if (flags & MS_QUEUE_NEXT_ON_MISS)
            vbl.request.type |= DRM_VBLANK_NEXTONMISS;

        vbl.request.sequence = msc;
        vbl.request.signal = seq;
        ret = drmWaitVBlank(ms->fd, &vbl);
        if (ret == 0) {
            msc = ms_kernel_msc_to_crtc_msc(crtc, vbl.reply.sequence, FALSE);
            ms_drm_set_seq_queued(seq, msc);
            if (msc_queued)
                *msc_queued = msc;
            return TRUE;
        }
    check:
        if (errno == EOPNOTSUPP) {
            ms_emulate_vblank(ms, scrn);
            continue;
        }
        if (errno != EBUSY) {
            ms_drm_abort_seq(scrn, seq);
            return FALSE;
        }
        ms_flush_drm_events(screen);
    }
}

/**
 * Convert a 32-bit or 64-bit kernel MSC sequence number to a 64-bit local
 * sequence number, adding in the high 32 bits, and dealing with 32-bit
 * wrapping if needed.
 */
uint64_t
ms_kernel_msc_to_crtc_msc(xf86CrtcPtr crtc, uint64_t sequence, Bool is64bit)
{
    drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;

    if (!is64bit) {
        /* sequence is provided as a 32 bit value from one of the 32 bit apis,
         * e.g., drmWaitVBlank(), classic vblank events, or pageflip events.
         *
         * Track and handle 32-Bit wrapping, somewhat robust against occasional
         * out-of-order not always monotonically increasing sequence values.
         */
        if ((int64_t) sequence < ((int64_t) drmmode_crtc->msc_prev - 0x40000000))
            drmmode_crtc->msc_high += 0x100000000L;

        if ((int64_t) sequence > ((int64_t) drmmode_crtc->msc_prev + 0x40000000))
            drmmode_crtc->msc_high -= 0x100000000L;

        drmmode_crtc->msc_prev = sequence;

        return drmmode_crtc->msc_high + sequence;
    }

    /* True 64-Bit sequence from Linux 4.15+ 64-Bit drmCrtcGetSequence /
     * drmCrtcQueueSequence apis and events. Pass through sequence unmodified,
     * but update the 32-bit tracking variables with reliable ground truth.
     *
     * With 64-Bit api in use, the only !is64bit input is from pageflip events,
     * and any pageflip event is usually preceded by some is64bit input from
     * swap scheduling, so this should provide reliable mapping for pageflip
     * events based on true 64-bit input as baseline as well.
     */
    drmmode_crtc->msc_prev = sequence;
    drmmode_crtc->msc_high = sequence & 0xffffffff00000000;

    return sequence;
}

int
ms_get_crtc_ust_msc(xf86CrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
    ScreenPtr screen = crtc->randr_crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    uint64_t kernel_msc;

    if (!ms_get_kernel_ust_msc(crtc, &kernel_msc, ust))
        return BadMatch;
    *msc = ms->emulate_vblank ? kernel_msc :
        ms_kernel_msc_to_crtc_msc(crtc, kernel_msc, ms->has_queue_sequence);

    return Success;
}

/**
 * Check for pending DRM events and process them.
 */
static void
ms_drm_socket_handler(int fd, int ready, void *data)
{
    ScreenPtr screen = data;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);

    if (data == NULL)
        return;

    drmHandleEvent(fd, &ms->event_context);
}

/*
 * Enqueue a potential drm response; when the associated response
 * appears, we've got data to pass to the handler from here
 */
uint32_t
ms_drm_queue_alloc(xf86CrtcPtr crtc,
                   void *data,
                   ms_drm_handler_proc handler,
                   ms_drm_abort_proc abort)
{
    ScreenPtr screen = crtc->randr_crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct ms_drm_queue *q;

    q = calloc(1, sizeof(struct ms_drm_queue));

    if (!q)
        return 0;
    if (!ms_drm_seq)
        ++ms_drm_seq;
    q->seq = ms_drm_seq++;
    q->msc = UINT64_MAX;
    q->scrn = scrn;
    q->crtc = crtc;
    q->data = data;
    q->handler = handler;
    q->abort = abort;

    /* Keep the list formatted in ascending order of sequence number */
    xorg_list_append(&q->list, &ms_drm_queue);

    return q->seq;
}

/**
 * Abort one queued DRM entry, removing it
 * from the list, calling the abort function and
 * freeing the memory
 */
static void
ms_drm_abort_one(struct ms_drm_queue *q)
{
    if (q->aborted)
        return;

    /* Don't remove vblank events if they were queued in the kernel */
    if (q->kernel_queued) {
        q->abort(q->data);
        q->aborted = TRUE;
    } else {
        xorg_list_del(&q->list);
        q->abort(q->data);
        free(q);
    }
}

/**
 * Abort all queued entries on a specific scrn, used
 * when resetting the X server
 */
static void
ms_drm_abort_scrn(ScrnInfoPtr scrn)
{
    struct ms_drm_queue *q, *tmp;

    xorg_list_for_each_entry_safe(q, tmp, &ms_drm_queue, list) {
        if (q->scrn == scrn)
            ms_drm_abort_one(q);
    }
}

/**
 * Abort by drm queue sequence number.
 */
void
ms_drm_abort_seq(ScrnInfoPtr scrn, uint32_t seq)
{
    struct ms_drm_queue *q, *tmp;

    xorg_list_for_each_entry_safe(q, tmp, &ms_drm_queue, list) {
        if (q->seq == seq) {
            ms_drm_abort_one(q);
            break;
        }
    }
}

/*
 * Externally usable abort function that uses a callback to match a single
 * queued entry to abort
 */
void
ms_drm_abort(ScrnInfoPtr scrn, Bool (*match)(void *data, void *match_data),
             void *match_data)
{
    struct ms_drm_queue *q;

    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (!q->aborted && match(q->data, match_data)) {
            ms_drm_abort_one(q);
            break;
        }
    }
}

/*
 * General DRM kernel handler. Looks for the matching sequence number in the
 * drm event queue and calls the handler for it.
 */
static uint64_t
ms_event_msc(xf86CrtcPtr crtc, uint64_t frame, uint64_t ns, Bool is64bit)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);

    /* With emulated vblanks only page flip events come from the kernel */
    if (ms->emulate_vblank)
        return is64bit ? frame : ms_vblank_emul_flipped(crtc, ns / 1000);
    return ms_kernel_msc_to_crtc_msc(crtc, frame, is64bit);
}

static void
ms_drm_sequence_handler(int fd, uint64_t frame, uint64_t ns, Bool is64bit, uint64_t user_data)
{
    struct ms_drm_queue *q, *tmp;
    uint32_t seq = (uint32_t) user_data;
    xf86CrtcPtr crtc = NULL;
    drmmode_crtc_private_ptr drmmode_crtc;
    uint64_t msc, next_msc = UINT64_MAX;

    /* Handle the seq for this event first in order to get the CRTC */
    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (q->seq == seq) {
            crtc = q->crtc;
            msc = ms_event_msc(crtc, frame, ns, is64bit);

            /* Write the current MSC to this event to ensure its handler runs in
             * the loop below. This is done because we don't want to run the
             * handler right now, since we need to ensure all events are handled
             * in FIFO order with respect to one another. Otherwise, if this
             * event were handled first just because it was queued to the
             * kernel, it could run before older events expiring at this MSC.
             */
            q->msc = msc;
            break;
        }
    }

    if (!crtc)
        return;

    /* Now run all of the vblank events for this CRTC with an expired MSC */
    xorg_list_for_each_entry_safe(q, tmp, &ms_drm_queue, list) {
        if (q->crtc == crtc && q->msc <= msc) {
            xorg_list_del(&q->list);
            if (!q->aborted)
                q->handler(msc, ns / 1000, q->data);
            free(q);
        }
    }

    /* Find this CRTC's next queued MSC and next non-queued MSC to be handled */
    msc = UINT64_MAX;
    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (q->crtc == crtc) {
            if (q->kernel_queued) {
                if (q->msc < next_msc)
                    next_msc = q->msc;
            } else if (q->msc < msc) {
                msc = q->msc;
                seq = q->seq;
            }
        }
    }

    /* Queue an event if the next queued MSC isn't soon enough */
    drmmode_crtc = crtc->driver_private;
    drmmode_crtc->next_msc = next_msc;
    if (msc < next_msc && !ms_queue_vblank(crtc, MS_QUEUE_ABSOLUTE, msc, NULL, seq)) {
        xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
                   "failed to queue next vblank event, aborting lost events\n");
        xorg_list_for_each_entry_safe(q, tmp, &ms_drm_queue, list) {
            if (q->crtc == crtc && q->msc < next_msc)
                ms_drm_abort_one(q);
        }
    }
}

static void
ms_drm_sequence_handler_64bit(int fd, uint64_t frame, uint64_t ns, uint64_t user_data)
{
    /* frame is true 64 bit wrapped into 64 bit */
    ms_drm_sequence_handler(fd, frame, ns, TRUE, user_data);
}

static void
ms_drm_handler(int fd, uint32_t frame, uint32_t sec, uint32_t usec,
               void *user_ptr)
{
    /* frame is 32 bit wrapped into 64 bit */
    ms_drm_sequence_handler(fd, frame, ((uint64_t) sec * 1000000 + usec) * 1000,
                            FALSE, (uint32_t) (uintptr_t) user_ptr);
}

Bool
ms_drm_queue_is_empty(void)
{
    return xorg_list_is_empty(&ms_drm_queue);
}

Bool
ms_vblank_screen_init(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    modesettingEntPtr ms_ent = ms_ent_priv(scrn);
    xorg_list_init(&ms_drm_queue);

    ms->event_context.version = 4;
    ms->event_context.vblank_handler = ms_drm_handler;
    ms->event_context.page_flip_handler = ms_drm_handler;
    ms->event_context.sequence_handler = ms_drm_sequence_handler_64bit;

    /* We need to re-register the DRM fd for the synchronisation
     * feedback on every server generation, so perform the
     * registration within ScreenInit and not PreInit.
     */
    if (ms_ent->fd_wakeup_registered != serverGeneration) {
        SetNotifyFd(ms->fd, ms_drm_socket_handler, X_NOTIFY_READ, screen);
        ms_ent->fd_wakeup_registered = serverGeneration;
        ms_ent->fd_wakeup_ref = 1;
    } else
        ms_ent->fd_wakeup_ref++;

    return TRUE;
}

void
ms_vblank_close_screen(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    modesettingPtr ms = modesettingPTR(scrn);
    modesettingEntPtr ms_ent = ms_ent_priv(scrn);
    int c;

    ms_drm_abort_scrn(scrn);

    for (c = 0; c < config->num_crtc; c++)
        ms_vblank_crtc_fini(config->crtc[c]);

    if (ms_ent->fd_wakeup_registered == serverGeneration &&
        !--ms_ent->fd_wakeup_ref) {
        RemoveNotifyFd(ms->fd);
    }
}
