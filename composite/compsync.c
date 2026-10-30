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
#include "xace.h"
#include "xacestr.h"

#define COMP_SERVER_FRAME_TIMEOUT_MS 200
#define COMP_SERVER_GEOMETRY_MASK (CWX | CWY | CWWidth | CWHeight | CWBorderWidth)

static int (*compServerConfigureWindow) (ClientPtr client);
static int (*compServerSendEvent) (ClientPtr client);
static Bool compServerApplyQueued;

static Bool
compServerHasWmState(WindowPtr pWin)
{
    PropertyPtr pProp;

    return dixLookupProperty(&pProp, pWin, compServerAtom("WM_STATE"),
                             serverClient, DixReadAccess) == Success;
}

static WindowPtr
compServerFindClientWindow(WindowPtr pParent, int depth)
{
    WindowPtr pChild, pClient;

    for (pChild = pParent->firstChild; pChild; pChild = pChild->nextSib) {
        if (compServerHasWmState(pChild))
            return pChild;
        if (depth > 1 &&
            (pClient = compServerFindClientWindow(pChild, depth - 1)))
            return pClient;
    }
    return NULL;
}

static WindowPtr
compServerClientWindow(WindowPtr pToplevel)
{
    WindowPtr pClient;

    if (compServerHasWmState(pToplevel))
        return pToplevel;

    pClient = compServerFindClientWindow(pToplevel, 2);
    return pClient ? pClient : pToplevel;
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
compServerNoteClientResized(CompServerWindowPtr sw)
{
    CompServerFrameWaitRec *wait = &sw->wait;

    if (wait->active || wait->sync.waiting)
        wait->clientResized = TRUE;
}

static void
compServerEndWait(CompServerWindowPtr sw)
{
    sw->wait.clientResized = FALSE;
    if (compServerHasDeferred(&sw->wait.deferred))
        compServerQueueApply();
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
    compServerEndWait(sw);
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
    WindowPtr pClient = compServerClientWindow(sw->pWin);

    sync->waiting = FALSE;
    if (!sw->wait.active) {
        sync->sent = FALSE;
        compServerEndWait(sw);
    }
    else if (!present_window_has_pending_frames(pClient))
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
compServerWaitForSync(CompServerWindowPtr sw, SyncCounter *pCounter,
                      int64_t value)
{
    CompServerSyncRequestRec *sync = &sw->wait.sync;

    compServerStopSyncWait(sw);
    sw->wait.clientResized = FALSE;
    sync->value = value;
    sync->sent = TRUE;
    sync->waiting = TRUE;
    sync->trigger = (SyncTrigger) {
        .pSync = &pCounter->sync,
        .test_value = value,
        .CheckTrigger = compServerSyncCheck,
        .TriggerFired = compServerSyncFired,
        .CounterDestroyed = compServerSyncCounterDestroyed,
    };
    SyncAddTriggerToSyncObject(&sync->trigger);
    compServerArmFrameTimeout(sw);
}

static void
compServerSendSyncRequest(CompServerWindowPtr sw)
{
    WindowPtr pWin = compServerClientWindow(sw->pWin);
    CompServerSyncRequestRec *sync = &sw->wait.sync;
    SyncCounter *pCounter = compServerSyncCounter(pWin);
    xEvent event = { .u.clientMessage.window = pWin->drawable.id };

    if (!pCounter)
        return;

    compServerWaitForSync(sw, pCounter, max(sync->value, pCounter->value) + 1);

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
compServerApplyDeferred(CompServerWindowPtr sw)
{
    CompServerDeferredWmPtr deferred = &sw->wait.deferred;
    WindowPtr pClient = compServerClientWindow(sw->pWin);
    Bool resizes;

    if (!compServerHasDeferred(deferred))
        return;

    resizes = compServerDeferredResizes(deferred, pClient);
    if (resizes)
        compServerSendSyncRequest(sw);

    compServerReplayDeferred(deferred);
    if (resizes)
        compServerNoteClientResized(sw);
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
                compServerApplyDeferred(sw);
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
compServerInitFrameWait(CompServerWindowPtr sw)
{
    compServerInitDeferred(&sw->wait.deferred);
}

void
compServerFreeFrameWait(CompServerWindowPtr sw)
{
    compServerStopSyncWait(sw);
    TimerFree(sw->wait.timer);
    sw->wait.timer = NULL;
    compServerFreeDeferred(&sw->wait.deferred);
}

void
CompositeServerPresented(WindowPtr pWin, PixmapPtr pPixmap)
{
    CompServerWindowPtr sw;
    Bool matches;

    if (!pWin->parent || !CompositeServerActive(pWin->drawable.pScreen))
        return;

    sw = GetCompServerWindow(compServerToplevel(pWin));
    if (!sw || pWin != compServerClientWindow(sw->pWin))
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
    else if (!matches && pWin == sw->pWin)
        compServerUpdateHeldFrame(sw, pPixmap);
}

static Bool
compServerIsWm(ClientPtr client, WindowPtr pRoot)
{
    OtherClientsPtr other;

    for (other = wOtherClients(pRoot); other; other = other->next) {
        if (other->mask & SubstructureRedirectMask)
            return CLIENT_ID(other->resource) == client->index;
    }
    return FALSE;
}

static CompServerWindowPtr
compServerWmTarget(ClientPtr client, XID id, WindowPtr *ppWin)
{
    WindowPtr pWin, pToplevel;
    CompServerWindowPtr sw;

    if (dixLookupWindow(&pWin, id, client, DixGetAttrAccess) != Success ||
        !pWin->parent || !CompositeServerActive(pWin->drawable.pScreen) ||
        !compServerIsWm(client, pWin->drawable.pScreen->root))
        return NULL;

    pToplevel = compServerToplevel(pWin);
    sw = GetCompServerWindow(pToplevel);
    if (!sw || pToplevel->overrideRedirect ||
        !compServerHasWmState(compServerClientWindow(pToplevel)))
        return NULL;

    *ppWin = pWin;
    return sw;
}

static Bool
compServerShouldDefer(CompServerWindowPtr sw, Bool startsStep)
{
    CompServerFrameWaitRec *wait = &sw->wait;

    return compServerHasDeferred(&wait->deferred) ||
        (startsStep && wait->clientResized &&
         (wait->active || wait->sync.waiting));
}

static void
compServerHideSyncCounter(CallbackListPtr *pcbl, void *closure, void *data)
{
    XacePropertyAccessRec *rec = data;
    WindowPtr pWin = rec->pWin;

    if (!(rec->access_mode & ~(DixReadAccess | DixGetAttrAccess)) &&
        (*rec->ppProp)->propertyName ==
        compServerAtom("_NET_WM_SYNC_REQUEST_COUNTER") &&
        CompositeServerActive(pWin->drawable.pScreen) &&
        compServerIsWm(rec->client, pWin->drawable.pScreen->root))
        rec->status = BadMatch;
}

void
compServerDeliverWmEvent(ClientPtr client, WindowPtr pWin, Mask eventMask,
                         xEvent *event)
{
    event->u.u.type |= 0x80;
    DeliverEventsToWindow(PickPointer(client), pWin, event, 1, eventMask,
                          NullGrab);
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
    WindowPtr pWin, pClient;
    Bool resizes;
    int rc;

    if (client->req_len >= bytes_to_int32(sizeof(*stuff)) &&
        Ones(stuff->mask) == client->req_len - bytes_to_int32(sizeof(*stuff)))
        sw = compServerWmTarget(client, stuff->window, &pWin);

    if (!sw)
        return (*compServerConfigureWindow) (client);

    pClient = compServerClientWindow(sw->pWin);
    resizes = (pWin == sw->pWin || pWin == pClient) &&
        compServerConfigureResizes(client, pWin);

    if (compServerShouldDefer(sw, resizes)) {
        if (!(stuff->mask & ~COMP_SERVER_GEOMETRY_MASK) &&
            compServerDeferConfigure(&sw->wait.deferred, client, pWin,
                                     stuff->mask, (CARD32 *) &stuff[1]))
            return Success;
        compServerApplyDeferred(sw);
    }
    else if (resizes && pWin == pClient && !sw->wait.sync.waiting)
        compServerSendSyncRequest(sw);

    rc = (*compServerConfigureWindow) (client);
    if (rc == Success && resizes && pWin == pClient)
        compServerNoteClientResized(sw);
    return rc;
}

static int
compServerProcSendEvent(ClientPtr client)
{
    REQUEST(xSendEventReq);
    CompServerWindowPtr sw = NULL;
    WindowPtr pWin;

    if (client->req_len == bytes_to_int32(sizeof(*stuff)) &&
        stuff->propagate == xFalse &&
        (stuff->event.u.u.type & 0x7f) == ConfigureNotify)
        sw = compServerWmTarget(client, stuff->destination, &pWin);

    if (sw && compServerShouldDefer(sw, FALSE) &&
        compServerDeferEvent(&sw->wait.deferred, client, pWin,
                             stuff->eventMask, &stuff->event))
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
            if (sw->wait.deferred.client == client)
                compServerFreeDeferred(&sw->wait.deferred);
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

    if (!XaceRegisterCallback(XACE_PROPERTY_ACCESS, compServerHideSyncCounter,
                              NULL)) {
        DeleteCallback(&ClientStateCallback, compServerClientState, NULL);
        return FALSE;
    }

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
    XaceDeleteCallback(XACE_PROPERTY_ACCESS, compServerHideSyncCounter, NULL);
    DeleteCallback(&ClientStateCallback, compServerClientState, NULL);
}
