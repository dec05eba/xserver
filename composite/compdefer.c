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
#include <dix-config.h>
#endif

#include "compint.h"

static Bool
compServerSameEvent(const xEvent *a, const xEvent *b)
{
    int type = a->u.u.type & 0x7f;

    if (type != (b->u.u.type & 0x7f))
        return FALSE;

    return type != ClientMessage ||
        (a->u.clientMessage.u.l.type == b->u.clientMessage.u.l.type &&
         a->u.clientMessage.u.l.longs0 == b->u.clientMessage.u.l.longs0);
}

static CompServerDeferredOpPtr
compServerFindDeferredOp(CompServerDeferredWmPtr deferred,
                         CompServerDeferredKind kind, XID window,
                         const xEvent *event)
{
    CompServerDeferredOpPtr op;

    xorg_list_for_each_entry(op, &deferred->ops, entry) {
        if (op->kind == kind && op->window == window &&
            (kind != COMP_SERVER_DEFERRED_EVENT ||
             compServerSameEvent(&op->u.event.event, event)))
            return op;
    }
    return NULL;
}

static CompServerDeferredOpPtr
compServerAddDeferredOp(CompServerDeferredWmPtr deferred, ClientPtr client,
                        CompServerDeferredKind kind, XID window,
                        const xEvent *event)
{
    CompServerDeferredOpPtr op;

    if (deferred->client && deferred->client != client)
        return NULL;

    op = compServerFindDeferredOp(deferred, kind, window, event);
    if (!op) {
        op = calloc(1, sizeof(*op));
        if (!op)
            return NULL;
        op->kind = kind;
        op->window = window;
        xorg_list_append(&op->entry, &deferred->ops);
    }

    deferred->client = client;
    return op;
}

static void
compServerReplayConfigure(WindowPtr pWin,
                          const CompServerDeferredConfigureRec *configure,
                          ClientPtr client)
{
    XID values[5];
    int n = 0;

    if (configure->mask & CWX)
        values[n++] = (XID) configure->x;
    if (configure->mask & CWY)
        values[n++] = (XID) configure->y;
    if (configure->mask & CWWidth)
        values[n++] = configure->width;
    if (configure->mask & CWHeight)
        values[n++] = configure->height;
    if (configure->mask & CWBorderWidth)
        values[n++] = configure->borderWidth;

    ConfigureWindow(pWin, configure->mask, values, client);
}

void
compServerInitDeferred(CompServerDeferredWmPtr deferred)
{
    deferred->client = NULL;
    xorg_list_init(&deferred->ops);
}

Bool
compServerHasDeferred(CompServerDeferredWmPtr deferred)
{
    return !xorg_list_is_empty(&deferred->ops);
}

Bool
compServerDeferConfigure(CompServerDeferredWmPtr deferred, ClientPtr client,
                         WindowPtr pWin, Mask mask, const CARD32 *values)
{
    CompServerDeferredOpPtr op;
    CompServerDeferredConfigureRec *configure;

    op = compServerAddDeferredOp(deferred, client,
                                 COMP_SERVER_DEFERRED_CONFIGURE,
                                 pWin->drawable.id, NULL);
    if (!op)
        return FALSE;

    configure = &op->u.configure;
    if (mask & CWX)
        configure->x = (INT16) *values++;
    if (mask & CWY)
        configure->y = (INT16) *values++;
    if (mask & CWWidth)
        configure->width = (CARD16) *values++;
    if (mask & CWHeight)
        configure->height = (CARD16) *values++;
    if (mask & CWBorderWidth)
        configure->borderWidth = (CARD16) *values;
    configure->mask |= mask;
    return TRUE;
}

Bool
compServerDeferEvent(CompServerDeferredWmPtr deferred, ClientPtr client,
                     WindowPtr pWin, Mask eventMask, const xEvent *event)
{
    CompServerDeferredOpPtr op;

    op = compServerAddDeferredOp(deferred, client, COMP_SERVER_DEFERRED_EVENT,
                                 pWin->drawable.id, event);
    if (!op)
        return FALSE;

    op->u.event.eventMask = eventMask;
    op->u.event.event = *event;
    return TRUE;
}

Bool
compServerDeferredResizes(CompServerDeferredWmPtr deferred, WindowPtr pWin)
{
    CompServerDeferredOpPtr op;
    CompServerDeferredConfigureRec *configure;

    op = compServerFindDeferredOp(deferred, COMP_SERVER_DEFERRED_CONFIGURE,
                                  pWin->drawable.id, NULL);
    if (!op)
        return FALSE;

    configure = &op->u.configure;
    return ((configure->mask & CWWidth) &&
            configure->width != pWin->drawable.width) ||
        ((configure->mask & CWHeight) &&
         configure->height != pWin->drawable.height);
}

void
compServerReplayDeferred(CompServerDeferredWmPtr deferred)
{
    ClientPtr client = deferred->client;
    CompServerDeferredOpPtr op;
    WindowPtr pWin;

    deferred->client = NULL;
    while (compServerHasDeferred(deferred)) {
        op = xorg_list_first_entry(&deferred->ops, CompServerDeferredOpRec,
                                   entry);
        xorg_list_del(&op->entry);
        if (dixLookupWindow(&pWin, op->window, serverClient,
                            DixGetAttrAccess) == Success) {
            if (op->kind == COMP_SERVER_DEFERRED_CONFIGURE)
                compServerReplayConfigure(pWin, &op->u.configure, client);
            else
                compServerDeliverWmEvent(client, pWin, op->u.event.eventMask,
                                         &op->u.event.event);
        }
        free(op);
    }
}

void
compServerFreeDeferred(CompServerDeferredWmPtr deferred)
{
    CompServerDeferredOpPtr op, tmp;

    xorg_list_for_each_entry_safe(op, tmp, &deferred->ops, entry) {
        xorg_list_del(&op->entry);
        free(op);
    }
    deferred->client = NULL;
}
