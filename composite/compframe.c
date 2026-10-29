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

#include "compint.h"
#include "present.h"
#include "propertyst.h"
#include "syncsrv.h"

#define COMP_SERVER_HIDDEN_FRAME_US 100000

static Bool compServerAdvertiseQueued;

static CompServerFrameClientPtr
compServerFrameClientFromTrigger(SyncTrigger *pTrigger)
{
    return (CompServerFrameClientPtr) ((char *) pTrigger -
                                       offsetof(CompServerFrameClientRec,
                                                trigger));
}

static void
compServerSendFrameMessage(WindowPtr pWin, Atom type, const INT32 data[5])
{
    xEvent event = {
        .u.clientMessage.window = pWin->drawable.id,
        .u.clientMessage.u.l.type = type,
        .u.clientMessage.u.l.longs0 = data[0],
        .u.clientMessage.u.l.longs1 = data[1],
        .u.clientMessage.u.l.longs2 = data[2],
        .u.clientMessage.u.l.longs3 = data[3],
        .u.clientMessage.u.l.longs4 = data[4],
    };

    event.u.u.type = ClientMessage;
    event.u.u.detail = 32;
    TryClientEvents(wClient(pWin), NULL, &event, 1, NoEventMask, NoEventMask,
                    NullGrab);
}

static INT32
compServerPresentationOffset(const CompServerFrameAckRec *ack, uint64_t drawn)
{
    INT32 offset;

    if (!ack->interval)
        return 0;

    offset = (int64_t) (ack->presented - drawn);
    return offset ? offset : -1;
}

static void
compServerSendFrameAck(CompServerFrameClientPtr fc)
{
    CompServerFrameAckRec *ack = &fc->ack;
    uint64_t drawn = GetTimeInMicros();
    const INT32 drawnData[5] = {
        ack->value & 0xffffffff, ack->value >> 32,
        drawn & 0xffffffff, drawn >> 32, 0,
    };
    const INT32 timingsData[5] = {
        ack->value & 0xffffffff, ack->value >> 32,
        compServerPresentationOffset(ack, drawn), ack->interval, 0,
    };

    ack->scheduled = FALSE;
    compServerSendFrameMessage(fc->pWin, compServerAtom("_NET_WM_FRAME_DRAWN"),
                               drawnData);
    compServerSendFrameMessage(fc->pWin, compServerAtom("_NET_WM_FRAME_TIMINGS"),
                               timingsData);
}

static CARD32
compServerFrameAckTimeout(OsTimerPtr timer, CARD32 time, void *arg)
{
    compServerSendFrameAck(arg);
    return 0;
}

static Bool
compServerWindowOnScreen(WindowPtr pWin)
{
    WindowPtr pRoot = pWin->drawable.pScreen->root;
    BoxRec box = {
        .x1 = pWin->drawable.x,
        .y1 = pWin->drawable.y,
        .x2 = pWin->drawable.x + pWin->drawable.width,
        .y2 = pWin->drawable.y + pWin->drawable.height,
    };

    return RegionContainsRect(&pRoot->winSize, &box) != rgnOUT;
}

static void
compServerScheduleFrameAck(CompServerFrameClientPtr fc)
{
    CompServerFrameAckRec *ack = &fc->ack;
    uint64_t now = GetTimeInMicros();
    uint64_t ust, interval;

    if (compServerWindowOnScreen(fc->pWin) &&
        present_window_vblank_timing(fc->pWin, &ust, &interval) && interval) {
        ack->presented = ust > now ? ust :
            ust + ((now - ust) / interval + 1) * interval;
        ack->interval = interval;
    }
    else {
        ack->presented = now + COMP_SERVER_HIDDEN_FRAME_US;
        ack->interval = 0;
    }

    ack->timer = TimerSet(ack->timer, 0,
                          max((ack->presented - now + 999) / 1000, 1),
                          compServerFrameAckTimeout, fc);
    ack->scheduled = ack->timer != NULL;
    if (!ack->scheduled)
        compServerSendFrameAck(fc);
}

static Bool
compServerFrameCounterCheck(SyncTrigger *pTrigger, int64_t oldval)
{
    SyncCounter *pCounter = (SyncCounter *) pTrigger->pSync;

    return pCounter && pCounter->value != oldval && pCounter->value > 0 &&
        !(pCounter->value & 1);
}

static void
compServerFrameCounterFired(SyncTrigger *pTrigger)
{
    CompServerFrameClientPtr fc = compServerFrameClientFromTrigger(pTrigger);

    if (fc->ack.scheduled)
        compServerSendFrameAck(fc);
    fc->ack.value = ((SyncCounter *) pTrigger->pSync)->value;
    compServerScheduleFrameAck(fc);
}

static void
compServerFrameCounterDestroyed(SyncTrigger *pTrigger)
{
    pTrigger->pSync = NULL;
}

static void
compServerFreeFrameClient(CompServerFrameClientPtr fc)
{
    if (fc->ack.scheduled)
        compServerSendFrameAck(fc);
    if (fc->trigger.pSync)
        SyncDeleteTriggerFromSyncObject(&fc->trigger);
    TimerFree(fc->ack.timer);
    xorg_list_del(&fc->entry);
    free(fc);
}

static CompServerFrameClientPtr
compServerFindFrameClient(CompServerPtr server, WindowPtr pWin)
{
    CompServerFrameClientPtr fc;

    xorg_list_for_each_entry(fc, &server->frameTiming.clients, entry) {
        if (fc->pWin == pWin)
            return fc;
    }
    return NULL;
}

static void
compServerForgetFrameClient(CompServerPtr server, WindowPtr pWin)
{
    CompServerFrameClientPtr fc = compServerFindFrameClient(server, pWin);

    if (fc)
        compServerFreeFrameClient(fc);
}

static void
compServerFreeFrameClients(CompServerPtr server)
{
    CompServerFrameClientPtr fc, tmp;

    xorg_list_for_each_entry_safe(fc, tmp, &server->frameTiming.clients, entry)
        compServerFreeFrameClient(fc);
}

static SyncCounter *
compServerExtendedCounter(WindowPtr pWin)
{
    PropertyPtr pProp;
    SyncCounter *pCounter;

    if (dixLookupProperty(&pProp, pWin,
                          compServerAtom("_NET_WM_SYNC_REQUEST_COUNTER"),
                          serverClient, DixReadAccess) != Success ||
        pProp->format != 32 || pProp->size < 2 ||
        SyncLookupCounter(&pCounter, ((CARD32 *) pProp->data)[1],
                          serverClient) != Success)
        return NULL;

    return pCounter;
}

static void
compServerWatchFrameCounter(CompServerPtr server, WindowPtr pWin)
{
    CompServerFrameClientPtr fc = compServerFindFrameClient(server, pWin);
    SyncCounter *pCounter = compServerExtendedCounter(pWin);

    if (!pCounter) {
        compServerForgetFrameClient(server, pWin);
        return;
    }

    if (!fc) {
        fc = calloc(1, sizeof(*fc));
        if (!fc)
            return;
        fc->pWin = pWin;
        xorg_list_add(&fc->entry, &server->frameTiming.clients);
    }
    else if (fc->trigger.pSync == &pCounter->sync)
        return;
    else if (fc->trigger.pSync)
        SyncDeleteTriggerFromSyncObject(&fc->trigger);

    fc->trigger = (SyncTrigger) {
        .pSync = &pCounter->sync,
        .CheckTrigger = compServerFrameCounterCheck,
        .TriggerFired = compServerFrameCounterFired,
        .CounterDestroyed = compServerFrameCounterDestroyed,
    };
    SyncAddTriggerToSyncObject(&fc->trigger);
}

static int
compServerWatchExistingWindow(WindowPtr pWin, void *data)
{
    compServerWatchFrameCounter(data, pWin);
    return WT_WALKCHILDREN;
}

static void
compServerAdvertiseFrameTiming(ScreenPtr pScreen, CompServerPtr server)
{
    CompServerFrameTimingRec *timing = &server->frameTiming;
    Atom supported = compServerAtom("_NET_SUPPORTED");
    Atom atoms[2] = {
        compServerAtom("_NET_WM_FRAME_DRAWN"),
        compServerAtom("_NET_WM_FRAME_TIMINGS"),
    };
    int rc;

    timing->advertiseQueued = FALSE;
    if (!pScreen->root ||
        compServerPropertyHasAtom(pScreen->root, supported, atoms[0]))
        return;

    timing->appending = TRUE;
    rc = dixChangeWindowProperty(serverClient, pScreen->root, supported,
                                 XA_ATOM, 32, PropModeAppend, 2, atoms, TRUE);
    timing->appending = FALSE;

    if (rc == Success && !timing->active) {
        timing->active = TRUE;
        TraverseTree(pScreen->root, compServerWatchExistingWindow, server);
    }
}

static Bool
compServerAdvertiseWork(ClientPtr client, void *closure)
{
    int i;

    compServerAdvertiseQueued = FALSE;

    for (i = 0; i < screenInfo.numScreens; i++) {
        ScreenPtr pScreen = screenInfo.screens[i];
        CompServerPtr server = compServerGet(pScreen);

        if (server && server->frameTiming.advertiseQueued)
            compServerAdvertiseFrameTiming(pScreen, server);
    }
    return TRUE;
}

static void
compServerQueueAdvertise(CompServerPtr server)
{
    server->frameTiming.advertiseQueued = TRUE;
    if (!compServerAdvertiseQueued)
        compServerAdvertiseQueued = QueueWorkProc(compServerAdvertiseWork,
                                                  serverClient, NULL);
}

static void
compServerSupportedChanged(CompServerPtr server, WindowPtr pRoot)
{
    CompServerFrameTimingRec *timing = &server->frameTiming;

    if (timing->appending)
        return;

    if (compServerPropertyHasAtom(pRoot, compServerAtom("_NET_SUPPORTED"),
                                  compServerAtom("_NET_WM_FRAME_DRAWN"))) {
        timing->active = FALSE;
        compServerFreeFrameClients(server);
    }
    else
        compServerQueueAdvertise(server);
}

void
compServerFramePropertyChanged(CompServerPtr server, PropertyStateRec *rec)
{
    WindowPtr pWin = rec->win;
    Atom name = rec->prop->propertyName;

    if (name == compServerAtom("_NET_WM_SYNC_REQUEST_COUNTER")) {
        if (rec->state == PropertyDelete)
            compServerForgetFrameClient(server, pWin);
        else if (server->frameTiming.active)
            compServerWatchFrameCounter(server, pWin);
    }
    else if (name == compServerAtom("_NET_SUPPORTED") &&
             pWin == pWin->drawable.pScreen->root &&
             rec->state == PropertyNewValue)
        compServerSupportedChanged(server, pWin);
}

void
compServerInitFrameTiming(CompServerPtr server)
{
    xorg_list_init(&server->frameTiming.clients);
}

void
compServerCloseFrameTiming(CompServerPtr server)
{
    compServerFreeFrameClients(server);
}
