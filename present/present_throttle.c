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
#include "client.h"
#include "compositeext.h"
#include "inputstr.h"

#define PRESENT_THROTTLE_FOREGROUND_USEC 1000000
#define PRESENT_THROTTLE_MAX_INTERVAL_USEC 1000000
#define PRESENT_THROTTLE_ACTIVE_USEC 250000
#define PRESENT_THROTTLE_SAMPLE_USEC 250000

static WindowPtr
present_throttle_toplevel(WindowPtr window)
{
    while (window->parent && window->parent->parent)
        window = window->parent;
    return window;
}

static WindowPtr
present_throttle_focus(void)
{
    DeviceIntPtr keyboard = inputInfo.keyboard;
    WindowPtr focus;

    if (!keyboard)
        return NULL;

    if (keyboard->deviceGrab.grab)
        return keyboard->deviceGrab.grab->window;

    focus = keyboard->focus ? keyboard->focus->win : NoneWin;
    if (focus == PointerRootWin)
        return GetSpriteWindow(inputInfo.pointer);
    if (focus == NoneWin || focus == FollowKeyboardWin)
        return NULL;
    return focus;
}

static Bool
present_throttle_same_process(ClientPtr client, ClientPtr other)
{
    pid_t pid = GetClientPid(client);

    return client == other || (other && pid > 0 && pid == GetClientPid(other));
}

static Bool
present_throttle_foreground(present_client_priv_ptr priv, ClientPtr client, WindowPtr window, uint64_t now)
{
    WindowPtr focus = present_throttle_focus();

    if (CompositeServerWindowCaptured(window) ||
        (focus && (present_throttle_same_process(client, clients[CLIENT_ID(focus->drawable.id)]) ||
                   present_throttle_toplevel(focus) == present_throttle_toplevel(window))))
        priv->focused_usec = now;

    return priv->focused_usec && now - priv->focused_usec < PRESENT_THROTTLE_FOREGROUND_USEC;
}

static void
present_gpu_cost_update(present_gpu_cost_ptr cost, uint64_t frame_usec)
{
    if (frame_usec >= cost->frame_usec)
        cost->frame_usec = frame_usec;
    else
        cost->frame_usec -= (cost->frame_usec - frame_usec) / 4;
}

static void
present_gpu_cost_sample(present_gpu_cost_ptr cost, ClientPtr client, present_vblank_ptr vblank, uint64_t now)
{
    Bool baseline = cost->reported && cost->read_usec;
    uint64_t ns = 0;

    if (!cost->read_usec || now - cost->read_usec >= PRESENT_THROTTLE_SAMPLE_USEC) {
        cost->reported = present_gpu_time_read(&cost->time, GetClientPid(client), now, &ns);
        if (cost->reported && baseline && cost->frames && ns >= cost->read_ns)
            present_gpu_cost_update(cost, (ns - cost->read_ns) / 1000 / cost->frames);

        cost->read_usec = now;
        cost->read_ns = ns;
        cost->frames = 0;
    }

    if (!cost->reported)
        present_gpu_cost_update(cost, vblank->rendered_usec ? vblank->rendered_usec - vblank->requested_usec : 0);
    cost->frames++;
}

static Bool
present_throttle_enabled(present_throttle_ptr throttle)
{
    return throttle->background_gpu_usage < 100;
}

static uint64_t
present_throttle_interval(present_throttle_ptr throttle, present_client_priv_ptr priv, Bool hidden, uint64_t now)
{
    uint64_t interval = 0;

    if (present_throttle_enabled(throttle))
        interval = min(priv->gpu_cost.frame_usec * 100 / throttle->background_gpu_usage, PRESENT_THROTTLE_MAX_INTERVAL_USEC);

    if (hidden && now - priv->visible_usec >= throttle->hidden_frame_usec)
        interval = max(interval, throttle->hidden_frame_usec);

    return interval;
}

static Bool
present_throttle_long_frames(present_screen_priv_ptr screen_priv, present_client_priv_ptr priv, RRCrtcPtr crtc)
{
    uint64_t refresh_usec = present_crtc_frame_usec(crtc);

    if (!refresh_usec)
        refresh_usec = screen_priv->fake_interval;

    return present_throttle_enabled(&screen_priv->throttle) && priv->gpu_cost.frame_usec * 2 > refresh_usec;
}

static Bool
present_throttle_due(present_screen_priv_ptr screen_priv, present_client_priv_ptr priv, present_vblank_ptr vblank,
                     Bool hidden, uint64_t now)
{
    present_throttle_ptr throttle = &screen_priv->throttle;
    uint64_t since = now - priv->released_usec;

    if (since < present_throttle_interval(throttle, priv, hidden, now))
        return FALSE;

    return since >= PRESENT_THROTTLE_MAX_INTERVAL_USEC ||
        now - throttle->activity_usec >= PRESENT_THROTTLE_ACTIVE_USEC ||
        !present_throttle_long_frames(screen_priv, priv, vblank->crtc);
}

Bool
present_throttle_hold(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    WindowPtr window = vblank->window;
    ScreenPtr screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);
    ClientPtr client = vblank->pixmap ? clients[CLIENT_ID(vblank->pixmap->drawable.id)] : NULL;
    present_client_priv_ptr priv;
    uint64_t now;
    Bool hidden;

    if (!client || client == serverClient)
        return FALSE;

    priv = present_client_priv(client);
    now = GetTimeInMicros();
    hidden = CompositeServerWindowHidden(window);
    if (!hidden)
        priv->visible_usec = now;

    if (present_throttle_foreground(priv, client, window, now)) {
        screen_priv->throttle.activity_usec = now;
        priv->gpu_cost.read_usec = 0;
        return FALSE;
    }

    if (!present_throttle_due(screen_priv, priv, vblank, hidden, now) &&
        screen_priv->queue_vblank(screen, window, vblank->crtc, vblank->event_id, crtc_msc + 1) == Success) {
        vblank->throttled = TRUE;
        return TRUE;
    }

    priv->released_usec = now;
    if (present_throttle_enabled(&screen_priv->throttle))
        present_gpu_cost_sample(&priv->gpu_cost, client, vblank, now);
    return FALSE;
}
