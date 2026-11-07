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

#include <dix-config.h>

#include "present_priv.h"
#include "compositeext.h"

static Bool
present_throttle_visible(WindowPtr window)
{
    return window->viewable && !CompositeServerWindowHidden(window);
}

Bool
present_throttle_hold(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    WindowPtr window = vblank->window;
    ScreenPtr screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);
    ClientPtr client = vblank->pixmap ? clients[CLIENT_ID(vblank->pixmap->drawable.id)] : NULL;
    uint64_t hold = screen_priv->hidden_frame_usec;
    present_client_priv_ptr priv;
    uint64_t now;

    vblank->throttled = FALSE;
    if (!client || client == serverClient)
        return FALSE;

    priv = present_client_priv(client);
    now = GetTimeInMicros();
    if (present_throttle_visible(window)) {
        priv->visible_usec = now;
        return FALSE;
    }

    if (vblank->async || !hold || now - priv->visible_usec < hold)
        return FALSE;

    if (now - priv->released_usec >= hold) {
        priv->released_usec = now;
        return FALSE;
    }

    vblank->throttled = screen_priv->queue_vblank(screen, window, vblank->crtc, vblank->event_id,
                                                  crtc_msc + 1) == Success;
    return vblank->throttled;
}
