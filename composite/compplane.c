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
compServerRegionsOverlap(RegionPtr pA, RegionPtr pB)
{
    RegionRec overlap;
    Bool overlaps;

    if (RegionContainsRect(pA, RegionExtents(pB)) == rgnOUT)
        return FALSE;

    RegionNull(&overlap);
    RegionIntersect(&overlap, pA, pB);
    overlaps = RegionNotEmpty(&overlap);
    RegionUninit(&overlap);
    return overlaps;
}

static Bool
compServerToplevelOverlapped(WindowPtr pTop, RegionPtr pRegion)
{
    WindowPtr pAbove;

    for (pAbove = pTop->parent->firstChild; pAbove != pTop; pAbove = pAbove->nextSib) {
        if (compServerPaintsWindow(pAbove) &&
            compServerRegionsOverlap(&pAbove->borderSize, pRegion))
            return TRUE;
    }
    return FALSE;
}

WindowPtr
CompositeServerMovingWindow(ScreenPtr pScreen, CARD64 since)
{
    WindowPtr pWin;

    if (!compServerGet(pScreen) || !pScreen->root)
        return NULL;

    for (pWin = pScreen->root->firstChild; pWin; pWin = pWin->nextSib) {
        CompServerWindowPtr sw = GetCompServerWindow(pWin);

        if (!sw || !compServerPaintsWindow(pWin) || sw->movedAt <= since || sw->resizedAt > since)
            continue;

        if (sw->held.layers || compServerToplevelOverlapped(pWin, &pWin->borderSize))
            return NULL;
        return pWin;
    }
    return NULL;
}

void
CompositeServerSetPlaneWindow(ScreenPtr pScreen, WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pScreen);

    if (server)
        server->planeWindow = pWin;
}

Bool
compServerIsPlaneWindow(WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);

    return server && server->planeWindow == pWin;
}

Bool
compServerIsPresentPlaneWindow(WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);

    return server && server->presentPlaneWindow == pWin;
}

void
CompositeServerSetPresentPlaneWindow(ScreenPtr pScreen, WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pScreen);

    if (!server || server->presentPlaneWindow == pWin)
        return;

    if (server->presentPlaneWindow)
        compServerAddDamage(server, &server->presentPlaneWindow->borderSize);
    if (pWin)
        compServerAddDamage(server, &pWin->borderSize);
    server->presentPlaneWindow = pWin;
}

WindowPtr
CompositeServerPresentPlaneWindow(ScreenPtr pScreen)
{
    CompServerPtr server = compServerGet(pScreen);

    return server ? server->presentPlaneWindow : NULL;
}

Bool
CompositeServerWindowUnobscured(WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);
    WindowPtr pTop;
    CompServerWindowPtr sw;

    if (!server || !pWin->parent || !pWin->viewable || compServerWindowHasAlpha(pWin) ||
        !RegionEqual(&pWin->clipList, &pWin->winSize))
        return FALSE;

    pTop = compServerToplevel(pWin);
    sw = GetCompServerWindow(pTop);
    if (!sw || sw->held.layers || !compServerPaintsWindow(pTop) ||
        compServerWindowOpacity(server, pTop) != COMP_SERVER_OPAQUE)
        return FALSE;

    return !compServerToplevelOverlapped(pTop, &pWin->borderSize);
}

Bool
CompositeServerWindowResized(WindowPtr pWin, CARD64 since)
{
    CompServerWindowPtr sw;

    if (!pWin->parent)
        return FALSE;

    sw = GetCompServerWindow(compServerToplevel(pWin));
    return sw && sw->resizedAt > since;
}

WindowPtr
CompositeServerPlaneWindow(ScreenPtr pScreen)
{
    CompServerPtr server = compServerGet(pScreen);

    return server ? server->planeWindow : NULL;
}

uint64_t
CompositeServerWindowSerial(WindowPtr pWin)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    return sw ? sw->damageSerial : 0;
}

void
CompositeServerPaintWindow(PicturePtr pDst, int x, int y, WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);
    CompServerLayersRec layers;
    int i;

    if (!server || !compServerBuildWindowLayers(server, pWin, &layers))
        return;

    for (i = layers.count - 1; i >= 0; i--)
        compServerPaintLayer(server, &layers.layers[i], pDst, x, y, NULL);

    SetPictureClipRegion(pDst, 0, 0, NULL);
    compServerFreeLayers(&layers);
}
