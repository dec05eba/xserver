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
compServerCopyFrame(PixmapPtr pSrc, PixmapPtr pDst)
{
    GCPtr pGC = GetScratchGC(pDst->drawable.depth, pDst->drawable.pScreen);

    if (!pGC)
        return FALSE;

    ValidateGC(&pDst->drawable, pGC);
    (void) (*pGC->ops->CopyArea) (&pSrc->drawable, &pDst->drawable, pGC,
                                  0, 0, pSrc->drawable.width,
                                  pSrc->drawable.height, 0, 0);
    FreeScratchGC(pGC);
    return TRUE;
}

static PixmapPtr
compServerCopyPixmap(PixmapPtr pSrc)
{
    ScreenPtr pScreen = pSrc->drawable.pScreen;
    PixmapPtr pPixmap;

    pPixmap = (*pScreen->CreatePixmap) (pScreen, pSrc->drawable.width,
                                        pSrc->drawable.height,
                                        pSrc->drawable.depth,
                                        CREATE_PIXMAP_USAGE_BACKING_PIXMAP);
    if (pPixmap && !compServerCopyFrame(pSrc, pPixmap)) {
        (*pScreen->DestroyPixmap) (pPixmap);
        return NULL;
    }
    return pPixmap;
}

static void
compServerDamageHeldFrame(CompServerWindowPtr sw)
{
    compServerAddDamage(compServerGet(sw->pWin->drawable.pScreen),
                        &sw->held.region);
}

static Bool
compServerInitHeldFrame(CompServerHeldFrameRec *held, int count)
{
    held->layers = calloc(count, sizeof(*held->layers));
    if (!held->layers)
        return FALSE;

    held->count = 0;
    RegionNull(&held->region);
    return TRUE;
}

static void
compServerFiniHeldFrame(ScreenPtr pScreen, CompServerHeldFrameRec *held)
{
    int i;

    if (!held->layers)
        return;

    for (i = 0; i < held->count; i++) {
        (*pScreen->DestroyPixmap) (held->layers[i].src.pPixmap);
        RegionUninit(&held->layers[i].region);
    }
    free(held->layers);
    held->layers = NULL;
    held->count = 0;
    RegionUninit(&held->region);
}

static void
compServerAddHeldLayer(CompServerHeldFrameRec *held,
                       const CompServerSourceRec *src, RegionPtr pRegion)
{
    CompServerHeldLayerPtr layer = &held->layers[held->count++];

    layer->src = *src;
    RegionNull(&layer->region);
    RegionCopy(&layer->region, pRegion);
    RegionUnion(&held->region, &held->region, pRegion);
}

static PixmapPtr
compServerHeldLayerCopy(CompServerLayersPtr pLayers,
                        CompServerHeldFrameRec *held, int index)
{
    PixmapPtr pSrc = pLayers->layers[index].src.pPixmap;
    int i;

    for (i = 0; i < index; i++) {
        if (pLayers->layers[i].src.pPixmap == pSrc) {
            held->layers[i].src.pPixmap->refcnt++;
            return held->layers[i].src.pPixmap;
        }
    }
    return compServerCopyPixmap(pSrc);
}

void
compServerHoldFrame(CompServerWindowPtr sw)
{
    WindowPtr pWin = sw->pWin;
    ScreenPtr pScreen = pWin->drawable.pScreen;
    CompServerLayersRec layers;
    CompServerHeldFrameRec held;
    int i;

    if (sw->held.layers || !pWin->viewable ||
        !compServerBuildWindowLayers(compServerGet(pScreen), pWin, &layers))
        return;

    if (layers.count && compServerInitHeldFrame(&held, layers.count)) {
        for (i = 0; i < layers.count; i++) {
            CompServerSourceRec src = layers.layers[i].src;

            src.pPixmap = compServerHeldLayerCopy(&layers, &held, i);
            if (!src.pPixmap)
                break;
            compServerAddHeldLayer(&held, &src, &layers.layers[i].region);
        }

        if (i == layers.count)
            sw->held = held;
        else
            compServerFiniHeldFrame(pScreen, &held);
    }
    compServerFreeLayers(&layers);
}

void
compServerMoveHeldFrame(CompServerWindowPtr sw, int dx, int dy)
{
    CompServerHeldFrameRec *held = &sw->held;
    int i;

    for (i = 0; i < held->count; i++) {
        held->layers[i].src.x += dx;
        held->layers[i].src.y += dy;
        RegionTranslate(&held->layers[i].region, dx, dy);
    }
    RegionTranslate(&held->region, dx, dy);
}

static PixmapPtr
compServerHeldFrameCopy(CompServerHeldFrameRec *held, PixmapPtr pFrame)
{
    PixmapPtr pPixmap = held->count == 1 ? held->layers[0].src.pPixmap : NULL;

    if (pPixmap && pPixmap->refcnt == 1 &&
        pPixmap->drawable.width == pFrame->drawable.width &&
        pPixmap->drawable.height == pFrame->drawable.height &&
        compServerCopyFrame(pFrame, pPixmap)) {
        pPixmap->refcnt++;
        return pPixmap;
    }
    return compServerCopyPixmap(pFrame);
}

void
compServerUpdateHeldFrame(CompServerWindowPtr sw, PixmapPtr pFrame)
{
    WindowPtr pWin = sw->pWin;
    ScreenPtr pScreen = pWin->drawable.pScreen;
    CompServerSourceRec src = {
        .pWin = pWin,
        .x = pWin->drawable.x,
        .y = pWin->drawable.y,
        .hdr = compServerIsHdrWindow(compServerGet(pScreen), pWin),
    };
    CompServerHeldFrameRec held;
    BoxRec box;
    RegionRec region;

    if (!sw->held.layers || wBoundingShape(pWin))
        return;

    src.pPixmap = compServerHeldFrameCopy(&sw->held, pFrame);
    if (!src.pPixmap)
        return;

    if (!compServerInitHeldFrame(&held, 1)) {
        (*pScreen->DestroyPixmap) (src.pPixmap);
        return;
    }

    box.x1 = src.x;
    box.y1 = src.y;
    box.x2 = box.x1 + src.pPixmap->drawable.width;
    box.y2 = box.y1 + src.pPixmap->drawable.height;
    RegionInit(&region, &box, 1);
    compServerAddHeldLayer(&held, &src, &region);
    RegionUninit(&region);

    compServerDamageHeldFrame(sw);
    compServerFiniHeldFrame(pScreen, &sw->held);
    sw->held = held;
    compServerDamageHeldFrame(sw);
}

void
compServerReleaseHeldFrame(CompServerWindowPtr sw)
{
    ScreenPtr pScreen = sw->pWin->drawable.pScreen;

    if (!sw->held.layers)
        return;

    compServerDamageHeldFrame(sw);
    compServerAddDamage(compServerGet(pScreen), &sw->pWin->borderSize);
    compServerFreeHeldFrame(sw);
}

void
compServerFreeHeldFrame(CompServerWindowPtr sw)
{
    compServerFiniHeldFrame(sw->pWin->drawable.pScreen, &sw->held);
}

static Bool
compServerHeldFrameShows(CompServerWindowPtr sw, WindowPtr pWin)
{
    int i;

    for (i = 0; i < sw->held.count; i++) {
        if (sw->held.layers[i].src.pWin == pWin)
            return TRUE;
    }
    return FALSE;
}

void
compServerReleaseHeldWindow(CompServerPtr server, WindowPtr pWin)
{
    CompServerWindowPtr sw;

    xorg_list_for_each_entry(sw, &server->windows, entry) {
        if (compServerHeldFrameShows(sw, pWin))
            compServerReleaseHeldFrame(sw);
    }
}

Bool
compServerWindowHeld(WindowPtr pWin)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    return sw && sw->held.layers;
}

Bool
compServerHeldFrameOverlaps(WindowPtr pWin, BoxPtr pBox)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    return sw && sw->held.layers &&
        RegionContainsRect(&sw->held.region, pBox) != rgnOUT;
}

void
compServerDisplayedRegion(WindowPtr pWin, RegionPtr pRegion)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    RegionCopy(pRegion, sw && sw->held.layers ? &sw->held.region :
               &pWin->borderSize);
}
