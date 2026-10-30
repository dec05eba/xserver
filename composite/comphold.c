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

static void
compServerSetHeldPixmap(CompServerWindowPtr sw, PixmapPtr pPixmap,
                        int x, int y, RegionPtr pRegion)
{
    if (pPixmap != sw->held.pPixmap) {
        compServerFreeHeldFrame(sw);
        sw->held.pPixmap = pPixmap;
        RegionNull(&sw->held.region);
    }
    sw->held.x = x;
    sw->held.y = y;
    RegionCopy(&sw->held.region, pRegion);
}

static Bool
compServerCanHoldFrame(CompServerWindowPtr sw)
{
    WindowPtr pWin = sw->pWin;

    return pWin->viewable &&
        !compServerHasSublayers(compServerGet(pWin->drawable.pScreen), pWin);
}

void
compServerHoldFrame(CompServerWindowPtr sw, PixmapPtr pOld)
{
    PixmapPtr pPixmap;

    if (sw->held.pPixmap || !compServerCanHoldFrame(sw))
        return;

    pPixmap = compServerCopyPixmap(pOld);
    if (pPixmap)
        compServerSetHeldPixmap(sw, pPixmap, pOld->screen_x, pOld->screen_y,
                                &sw->pWin->borderSize);
}

void
compServerMoveHeldFrame(CompServerWindowPtr sw, int dx, int dy)
{
    sw->held.x += dx;
    sw->held.y += dy;
    RegionTranslate(&sw->held.region, dx, dy);
}

void
compServerUpdateHeldFrame(CompServerWindowPtr sw, PixmapPtr pFrame)
{
    WindowPtr pWin = sw->pWin;
    PixmapPtr pPixmap = sw->held.pPixmap;
    BoxRec box;
    RegionRec region;

    if (!pPixmap || wBoundingShape(pWin))
        return;

    if (pPixmap->drawable.width != pFrame->drawable.width ||
        pPixmap->drawable.height != pFrame->drawable.height)
        pPixmap = compServerCopyPixmap(pFrame);
    else if (!compServerCopyFrame(pFrame, pPixmap))
        pPixmap = NULL;

    if (!pPixmap)
        return;

    box.x1 = pWin->drawable.x;
    box.y1 = pWin->drawable.y;
    box.x2 = box.x1 + pPixmap->drawable.width;
    box.y2 = box.y1 + pPixmap->drawable.height;
    RegionInit(&region, &box, 1);

    compServerDamageHeldFrame(sw);
    compServerSetHeldPixmap(sw, pPixmap, box.x1, box.y1, &region);
    compServerDamageHeldFrame(sw);
    RegionUninit(&region);
}

void
compServerReleaseHeldFrame(CompServerWindowPtr sw)
{
    ScreenPtr pScreen = sw->pWin->drawable.pScreen;

    if (!sw->held.pPixmap)
        return;

    compServerDamageHeldFrame(sw);
    compServerAddDamage(compServerGet(pScreen), &sw->pWin->borderSize);
    compServerFreeHeldFrame(sw);
}

void
compServerFreeHeldFrame(CompServerWindowPtr sw)
{
    ScreenPtr pScreen = sw->pWin->drawable.pScreen;

    if (sw->held.pPixmap) {
        (*pScreen->DestroyPixmap) (sw->held.pPixmap);
        sw->held.pPixmap = NULL;
        RegionUninit(&sw->held.region);
    }
}

Bool
compServerWindowHeld(WindowPtr pWin)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    return sw && sw->held.pPixmap;
}

Bool
compServerHeldFrameOverlaps(WindowPtr pWin, BoxPtr pBox)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    return sw && sw->held.pPixmap &&
        RegionContainsRect(&sw->held.region, pBox) != rgnOUT;
}

void
compServerDisplayedRegion(WindowPtr pWin, RegionPtr pRegion)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    RegionCopy(pRegion, sw && sw->held.pPixmap ? &sw->held.region :
               &pWin->borderSize);
}

PixmapPtr
compServerDisplayedPixmap(WindowPtr pWin, int *x, int *y)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);
    PixmapPtr pPixmap;

    if (sw && sw->held.pPixmap) {
        *x = sw->held.x;
        *y = sw->held.y;
        return sw->held.pPixmap;
    }

    pPixmap = (*pWin->drawable.pScreen->GetWindowPixmap) (pWin);
    *x = pPixmap->screen_x;
    *y = pPixmap->screen_y;
    return pPixmap;
}
