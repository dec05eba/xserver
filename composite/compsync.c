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

#include <stddef.h>
#include <X11/Xatom.h>
#include <X11/Xproto.h>

#include "os/osdep.h"

#include "compint.h"
#include "compositeext.h"
#include "inputstr.h"
#include "present.h"
#include "propertyst.h"
#include "syncsrv.h"

#define COMP_SERVER_FRAME_TIMEOUT_MS 200
#define COMP_SERVER_GEOMETRY_MASK (CWX | CWY | CWWidth | CWHeight | CWBorderWidth)

static int (*compServerConfigureWindow) (ClientPtr client);
static int (*compServerSendEvent) (ClientPtr client);
static Bool compServerApplyQueued;

static Bool
compServerWmSyncsResizes(ScreenPtr pScreen)
{
    return compServerPropertyHasAtom(pScreen->root,
                                     compServerAtom("_NET_SUPPORTED"),
                                     compServerAtom("_NET_WM_SYNC_REQUEST"));
}

static SyncCounter *
compServerSyncCounter(WindowPtr pWin)
{
    PropertyPtr pProp;
    SyncCounter *pCounter;

    if (!compServerPropertyHasAtom(pWin, compServerAtom("WM_PROTOCOLS"),
                                   compServerAtom("_NET_WM_SYNC_REQUEST")) ||
        dixLookupProperty(&pProp, pWin,
                          compServerAtom("_NET_WM_SYNC_REQUEST_COUNTER"),
                          serverClient, DixReadAccess) != Success ||
        pProp->format != 32 || pProp->size < 1 ||
        SyncLookupCounter(&pCounter, *(CARD32 *) pProp->data,
                          serverClient) != Success)
        return NULL;

    return pCounter;
}

static void
compServerQueueApply(void);

static void
compServerStopSyncWait(CompServerWindowPtr sw)
{
    CompServerSyncRequestRec *sync = &sw->wait.sync;

    if (sync->waiting && sync->trigger.pSync)
        SyncDeleteTriggerFromSyncObject(&sync->trigger);
    sync->waiting = FALSE;
}

static void
compServerFinishFrame(CompServerWindowPtr sw)
{
    CompServerFrameWaitRec *wait = &sw->wait;

    compServerReleaseHeldFrame(sw);
    TimerCancel(wait->timer);
    compServerStopSyncWait(sw);
    wait->sync.sent = FALSE;
    wait->active = FALSE;
    if (wait->pending.mask)
        compServerQueueApply();
}

static CARD32
compServerFrameTimeout(OsTimerPtr timer, CARD32 time, void *arg)
{
    compServerFinishFrame(arg);
    return 0;
}

static void
compServerArmFrameTimeout(CompServerWindowPtr sw)
{
    sw->wait.timer = TimerSet(sw->wait.timer, 0, COMP_SERVER_FRAME_TIMEOUT_MS,
                              compServerFrameTimeout, sw);
}

static CompServerWindowPtr
compServerSyncWindow(SyncTrigger *pTrigger)
{
    return (CompServerWindowPtr) ((char *) pTrigger -
                                  offsetof(CompServerWindowRec,
                                           wait.sync.trigger));
}

static void
compServerSyncReady(CompServerWindowPtr sw)
{
    CompServerSyncRequestRec *sync = &sw->wait.sync;

    sync->waiting = FALSE;
    if (!sw->wait.active)
        sync->sent = FALSE;
    else if (!present_window_has_pending_frames(sw->pWin))
        compServerFinishFrame(sw);
}

static Bool
compServerSyncCheck(SyncTrigger *pTrigger, int64_t oldval)
{
    SyncCounter *pCounter = (SyncCounter *) pTrigger->pSync;

    return pCounter && pCounter->value >= pTrigger->test_value;
}

static void
compServerSyncFired(SyncTrigger *pTrigger)
{
    SyncDeleteTriggerFromSyncObject(pTrigger);
    compServerSyncReady(compServerSyncWindow(pTrigger));
}

static void
compServerSyncCounterDestroyed(SyncTrigger *pTrigger)
{
    pTrigger->pSync = NULL;
    compServerSyncReady(compServerSyncWindow(pTrigger));
}

static void
compServerSendSyncRequest(CompServerWindowPtr sw)
{
    WindowPtr pWin = sw->pWin;
    CompServerSyncRequestRec *sync = &sw->wait.sync;
    SyncCounter *pCounter = compServerSyncCounter(pWin);
    xEvent event = { .u.clientMessage.window = pWin->drawable.id };

    if (!pCounter)
        return;

    compServerStopSyncWait(sw);
    if (sync->value < pCounter->value)
        sync->value = pCounter->value;
    sync->value++;
    sync->sent = TRUE;
    sync->waiting = TRUE;
    sync->trigger = (SyncTrigger) {
        .pSync = &pCounter->sync,
        .test_value = sync->value,
        .CheckTrigger = compServerSyncCheck,
        .TriggerFired = compServerSyncFired,
        .CounterDestroyed = compServerSyncCounterDestroyed,
    };
    SyncAddTriggerToSyncObject(&sync->trigger);
    compServerArmFrameTimeout(sw);

    event.u.u.type = ClientMessage;
    event.u.u.detail = 32;
    event.u.clientMessage.u.l.type = compServerAtom("WM_PROTOCOLS");
    event.u.clientMessage.u.l.longs0 = compServerAtom("_NET_WM_SYNC_REQUEST");
    event.u.clientMessage.u.l.longs1 = currentTime.milliseconds;
    event.u.clientMessage.u.l.longs2 = sync->value & 0xffffffff;
    event.u.clientMessage.u.l.longs3 = sync->value >> 32;
    TryClientEvents(wClient(pWin), NULL, &event, 1, NoEventMask, NoEventMask,
                    NullGrab);
}

static void
compServerApplyPending(CompServerWindowPtr sw)
{
    CompServerPendingConfigureRec pending = sw->wait.pending;
    WindowPtr pWin = sw->pWin;
    XID values[5];
    int n = 0;

    if (!pending.mask)
        return;

    sw->wait.pending.mask = 0;
    if (pending.mask & CWX)
        values[n++] = (XID) pending.x;
    if (pending.mask & CWY)
        values[n++] = (XID) pending.y;
    if (pending.mask & CWWidth)
        values[n++] = pending.width;
    if (pending.mask & CWHeight)
        values[n++] = pending.height;
    if (pending.mask & CWBorderWidth)
        values[n++] = pending.borderWidth;

    if (((pending.mask & CWWidth) && pending.width != pWin->drawable.width) ||
        ((pending.mask & CWHeight) && pending.height != pWin->drawable.height))
        compServerSendSyncRequest(sw);

    ConfigureWindow(pWin, pending.mask, values, pending.client);
}

static Bool
compServerApplyWork(ClientPtr client, void *closure)
{
    int i;

    compServerApplyQueued = FALSE;

    for (i = 0; i < screenInfo.numScreens; i++) {
        CompServerPtr server = compServerGet(screenInfo.screens[i]);
        CompServerWindowPtr sw;

        if (!server)
            continue;

        xorg_list_for_each_entry(sw, &server->windows, entry) {
            if (!sw->wait.active && !sw->wait.sync.waiting)
                compServerApplyPending(sw);
        }
    }
    return TRUE;
}

static void
compServerQueueApply(void)
{
    if (!compServerApplyQueued)
        compServerApplyQueued = QueueWorkProc(compServerApplyWork,
                                              serverClient, NULL);
}

void
compServerReconfigureFrame(WindowPtr pWin, PixmapPtr pOld, int x, int y,
                           int w, int h)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);
    CompServerFrameWaitRec *wait;

    if (!sw)
        return;

    if (w == pOld->drawable.width && h == pOld->drawable.height) {
        if (sw->held.pPixmap)
            compServerMoveHeldFrame(sw, x - pOld->screen_x,
                                    y - pOld->screen_y);
        return;
    }

    wait = &sw->wait;
    if (!wait->active && !sw->presentCount && !wait->sync.waiting)
        return;

    compServerHoldFrame(sw, pOld);
    wait->active = TRUE;
    compServerArmFrameTimeout(sw);
}

void
compServerFreeFrameWait(CompServerWindowPtr sw)
{
    compServerStopSyncWait(sw);
    TimerFree(sw->wait.timer);
    sw->wait.timer = NULL;
    sw->wait.pending.mask = 0;
}

void
CompositeServerPresented(WindowPtr pWin, PixmapPtr pPixmap)
{
    CompServerWindowPtr sw;
    Bool matches;

    if (!pWin->parent || pWin->parent->parent ||
        !CompositeServerActive(pWin->drawable.pScreen))
        return;

    sw = GetCompServerWindow(pWin);
    if (!sw)
        return;

    sw->presentCount++;
    if (!sw->wait.active)
        return;

    matches = pPixmap->drawable.width == pWin->drawable.width &&
        pPixmap->drawable.height == pWin->drawable.height;

    if (sw->wait.sync.sent ?
        !sw->wait.sync.waiting && !present_window_has_pending_frames(pWin) :
        matches)
        compServerFinishFrame(sw);
    else if (!matches)
        compServerUpdateHeldFrame(sw, pPixmap);
}

static Bool
compServerConfiguresDirectly(ClientPtr client, WindowPtr pWin)
{
    WindowPtr pParent = pWin->parent;
    OtherClientsPtr other;

    if (pWin->overrideRedirect)
        return TRUE;

    if (pParent->eventMask & SubstructureRedirectMask)
        return wClient(pParent) == client;

    for (other = wOtherClients(pParent); other; other = other->next) {
        if (other->mask & SubstructureRedirectMask)
            return CLIENT_ID(other->resource) == client->index;
    }
    return TRUE;
}

static CompServerWindowPtr
compServerWindowManagedBy(ClientPtr client, WindowPtr pWin)
{
    if (!pWin->parent || client->swapped ||
        CLIENT_ID(pWin->drawable.id) == client->index ||
        !compServerConfiguresDirectly(client, pWin) ||
        !CompositeServerActive(pWin->drawable.pScreen) ||
        compServerWmSyncsResizes(pWin->drawable.pScreen))
        return NULL;

    return GetCompServerWindow(pWin);
}

static void
compServerMergeConfigure(ClientPtr client, CompServerPendingConfigureRec *pending)
{
    REQUEST(xConfigureWindowReq);
    CARD32 *values = (CARD32 *) &stuff[1];
    Mask mask = stuff->mask;

    if (mask & CWX)
        pending->x = (INT16) *values++;
    if (mask & CWY)
        pending->y = (INT16) *values++;
    if (mask & CWWidth)
        pending->width = (CARD16) *values++;
    if (mask & CWHeight)
        pending->height = (CARD16) *values++;
    if (mask & CWBorderWidth)
        pending->borderWidth = (CARD16) *values;
    pending->mask |= mask;
    pending->client = client;
}

static Bool
compServerConfigureResizes(ClientPtr client, WindowPtr pWin)
{
    REQUEST(xConfigureWindowReq);
    CARD32 *values = (CARD32 *) &stuff[1];
    Mask mask = stuff->mask;

    if (mask & CWX)
        values++;
    if (mask & CWY)
        values++;
    if ((mask & CWWidth) && *values++ != pWin->drawable.width)
        return TRUE;
    return (mask & CWHeight) && *values != pWin->drawable.height;
}

static int
compServerProcConfigureWindow(ClientPtr client)
{
    REQUEST(xConfigureWindowReq);
    CompServerWindowPtr sw = NULL;
    WindowPtr pWin;

    if (client->req_len >= bytes_to_int32(sizeof(*stuff)) &&
        Ones(stuff->mask) == client->req_len - bytes_to_int32(sizeof(*stuff)) &&
        dixLookupWindow(&pWin, stuff->window, client,
                        DixGetAttrAccess) == Success)
        sw = compServerWindowManagedBy(client, pWin);

    if (!sw)
        return (*compServerConfigureWindow) (client);

    if (sw->wait.active || sw->wait.sync.waiting) {
        if (!(stuff->mask & ~COMP_SERVER_GEOMETRY_MASK)) {
            compServerMergeConfigure(client, &sw->wait.pending);
            return Success;
        }
        compServerApplyPending(sw);
    }
    else if (compServerConfigureResizes(client, pWin))
        compServerSendSyncRequest(sw);

    return (*compServerConfigureWindow) (client);
}

static int
compServerProcSendEvent(ClientPtr client)
{
    REQUEST(xSendEventReq);
    CompServerWindowPtr sw;
    WindowPtr pWin;

    if (client->req_len == bytes_to_int32(sizeof(*stuff)) &&
        (stuff->event.u.u.type & 0x7f) == ConfigureNotify &&
        stuff->destination == stuff->event.u.configureNotify.window &&
        dixLookupWindow(&pWin, stuff->destination, client,
                        DixGetAttrAccess) == Success &&
        (sw = compServerWindowManagedBy(client, pWin)) &&
        sw->wait.pending.mask)
        return Success;

    return (*compServerSendEvent) (client);
}

static void
compServerClientState(CallbackListPtr *pcbl, void *closure, void *data)
{
    ClientPtr client = ((NewClientInfoRec *) data)->client;
    int i;

    if (client->clientState != ClientStateGone)
        return;

    for (i = 0; i < screenInfo.numScreens; i++) {
        CompServerPtr server = compServerGet(screenInfo.screens[i]);
        CompServerWindowPtr sw;

        if (!server)
            continue;

        xorg_list_for_each_entry(sw, &server->windows, entry) {
            if (sw->wait.pending.client == client)
                sw->wait.pending.mask = 0;
        }
    }
}

Bool
compServerInitFrameSync(void)
{
    if (ProcVector[X_ConfigureWindow] == compServerProcConfigureWindow)
        return TRUE;

    if (!AddCallback(&ClientStateCallback, compServerClientState, NULL))
        return FALSE;

    compServerConfigureWindow = ProcVector[X_ConfigureWindow];
    ProcVector[X_ConfigureWindow] = compServerProcConfigureWindow;
    compServerSendEvent = ProcVector[X_SendEvent];
    ProcVector[X_SendEvent] = compServerProcSendEvent;
    return TRUE;
}

void
compServerCloseFrameSync(void)
{
    if (ProcVector[X_ConfigureWindow] != compServerProcConfigureWindow)
        return;

    ProcVector[X_ConfigureWindow] = compServerConfigureWindow;
    ProcVector[X_SendEvent] = compServerSendEvent;
    DeleteCallback(&ClientStateCallback, compServerClientState, NULL);
}
