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
#include <dix-config.h>

#include "present_priv.h"
#include "compositeext.h"
#include <unistd.h>
#ifdef DRI3
#include <sys/eventfd.h>
#endif /* DRI3 */

/*
 * Called when the wait fence is triggered; just gets the current msc/ust and
 * calls the proper execute again. That will re-check the fence and pend the
 * request again if it's still not actually ready
 */
static void
present_wait_fence_triggered(void *param)
{
    present_vblank_ptr      vblank = param;
    ScreenPtr               screen = vblank->screen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    screen_priv->re_execute(vblank);
}

#ifdef DRI3
static void present_syncobj_triggered(int fd, int xevents, void *data)
{
    present_vblank_ptr vblank = data;
    ScreenPtr screen = vblank->screen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    SetNotifyFd(fd, NULL, 0, NULL);
    close(fd);
    vblank->efd = -1;

    screen_priv->re_execute(vblank);
}
#endif /* DRI3 */

#define PRESENT_HIDDEN_MAX_HOLD_USEC 1000000

static uint64_t *
present_client_last_shown_usec(PixmapPtr pixmap)
{
    ClientPtr client = clients[CLIENT_ID(pixmap->drawable.id)];

    return client ? dixLookupPrivate(&client->devPrivates,
                                     &present_client_private_key) : NULL;
}

/* Clients like the GPU process of a browser present to all of their windows
 * from one thread, so holding back a hidden window would also stall the
 * visible ones.
 */
static Bool
present_wait_shown(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    WindowPtr window = vblank->window;
    ScreenPtr screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);
    uint64_t now = GetTimeInMicros(), *shown;

    if (!vblank->pixmap)
        return FALSE;

    shown = present_client_last_shown_usec(vblank->pixmap);
    if (!CompositeServerWindowHidden(window)) {
        vblank->hidden_since = 0;
        if (shown)
            *shown = now;
        return FALSE;
    }

    if (shown && *shown && now - *shown < PRESENT_HIDDEN_MAX_HOLD_USEC)
        return FALSE;

    if (!vblank->hidden_since)
        vblank->hidden_since = now;
    if (now - vblank->hidden_since >= PRESENT_HIDDEN_MAX_HOLD_USEC)
        return FALSE;

    return screen_priv->queue_vblank(screen, window, vblank->crtc,
                                     vblank->event_id, crtc_msc + 1) == Success;
}

static void
present_rendering_done(int fd, int xevents, void *data)
{
    present_vblank_ptr vblank = data;
    present_screen_priv_ptr screen_priv = present_screen_priv(vblank->screen);

    SetNotifyFd(fd, NULL, 0, NULL);
    close(fd);
    vblank->rendering_fd = -1;

    screen_priv->re_execute(vblank);
}

static Bool
present_wait_rendering(present_vblank_ptr vblank)
{
    ScreenPtr screen = vblank->window->drawable.pScreen;
    present_screen_info_ptr info = present_screen_priv(screen)->info;

    if (vblank->rendering_fd >= 0)
        return TRUE;
    if (!vblank->pixmap || !info || info->version < 2 ||
        !info->pixmap_rendering_fd)
        return FALSE;
#ifdef DRI3
    if (vblank->acquire_syncobj)
        return FALSE;
#endif /* DRI3 */

    vblank->rendering_fd = info->pixmap_rendering_fd(vblank->window,
                                                     vblank->pixmap);
    if (vblank->rendering_fd < 0)
        return FALSE;

    SetNotifyFd(vblank->rendering_fd, present_rendering_done, X_NOTIFY_READ,
                vblank);
    return TRUE;
}

Bool
present_execute_wait(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    /* We may have to requeue for the next MSC if check_flip_window prevented
     * using a flip.
     */
    if (vblank->exec_msc == crtc_msc + 1 &&
        screen_priv->queue_vblank(screen, window, vblank->crtc, vblank->event_id,
                                  vblank->exec_msc) == Success)
        return TRUE;

    if (vblank->wait_fence) {
        if (!present_fence_check_triggered(vblank->wait_fence)) {
            present_fence_set_callback(vblank->wait_fence, present_wait_fence_triggered, vblank);
            return TRUE;
        }
    }

#ifdef DRI3
    /* Defer execution of explicitly synchronized copies.
     * Flip synchronization is managed by the driver.
     */
    if (!vblank->flip && vblank->acquire_syncobj &&
        !vblank->acquire_syncobj->is_signaled(vblank->acquire_syncobj,
                                              vblank->acquire_point)) {
        vblank->efd = eventfd(0, EFD_CLOEXEC);
        SetNotifyFd(vblank->efd, present_syncobj_triggered, X_NOTIFY_READ, vblank);
        vblank->acquire_syncobj->signaled_eventfd(vblank->acquire_syncobj,
                                                  vblank->acquire_point,
                                                  vblank->efd);
        return TRUE;
    }
#endif /* DRI3 */

    return present_wait_shown(vblank, crtc_msc) ||
        present_wait_rendering(vblank);
}

void
present_execute_copy(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    /* If present_flip failed, we may have to requeue for the next MSC */
    if (vblank->exec_msc == crtc_msc + 1 &&
        Success == screen_priv->queue_vblank(screen,
                                             window,
                                             vblank->crtc,
                                             vblank->event_id,
                                             vblank->exec_msc)) {
        vblank->queued = TRUE;
        return;
    }

    present_copy_region(&window->drawable, vblank->pixmap, vblank->update, vblank->x_off, vblank->y_off);
    vblank->executed = TRUE;
    CompositeServerPresented(window, vblank->pixmap);

    /* present_copy_region sticks the region into a scratch GC,
     * which is then freed, freeing the region
     */
    vblank->update = NULL;
#ifdef DRI3
    if (vblank->release_syncobj) {
        int fence_fd = screen_priv->flush_fenced(window);
        vblank->release_syncobj->import_fence(vblank->release_syncobj,
                                              vblank->release_point, fence_fd);
    } else
#endif /* DRI3 */
    {
        screen_priv->flush(window);
        present_pixmap_idle(vblank->pixmap, vblank->window, vblank->serial, vblank->idle_fence);
    }
}

void
present_execute_post(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)
{
    uint8_t mode;

    /* Compute correct CompleteMode
     */
    if (vblank->kind == PresentCompleteKindPixmap) {
        if (vblank->pixmap && vblank->window) {
            if (present_vblank_suboptimal(vblank))
                mode = PresentCompleteModeSuboptimalCopy;
            else
                mode = PresentCompleteModeCopy;
        } else {
            mode = PresentCompleteModeSkip;
        }
    }
    else
        mode = PresentCompleteModeCopy;

    present_vblank_notify(vblank, vblank->kind, mode, ust, crtc_msc);
    present_vblank_destroy(vblank);
}
