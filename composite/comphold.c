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

static void
compServerDamageBox(ScreenPtr pScreen, BoxPtr pBox)
{
    RegionRec region;

    RegionInit(&region, pBox, 1);
    compServerAddDamage(compServerGet(pScreen), &region);
    RegionUninit(&region);
}

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
compServerSetHeldPixmap(CompServerWindowPtr sw, PixmapPtr pPixmap,
                        int x, int y)
{
    if (pPixmap != sw->held.pPixmap) {
        compServerFreeHeldFrame(sw);
        sw->held.pPixmap = pPixmap;
    }
    sw->held.box.x1 = x;
    sw->held.box.y1 = y;
    sw->held.box.x2 = x + pPixmap->drawable.width;
    sw->held.box.y2 = y + pPixmap->drawable.height;
}

static Bool
compServerCanHoldFrame(CompServerWindowPtr sw)
{
    WindowPtr pWin = sw->pWin;

    return pWin->viewable && !wBoundingShape(pWin) &&
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
        compServerSetHeldPixmap(sw, pPixmap, pOld->screen_x, pOld->screen_y);
}

void
compServerMoveHeldFrame(CompServerWindowPtr sw, int dx, int dy)
{
    BoxPtr pBox = &sw->held.box;

    pBox->x1 += dx;
    pBox->x2 += dx;
    pBox->y1 += dy;
    pBox->y2 += dy;
}

void
compServerUpdateHeldFrame(CompServerWindowPtr sw, PixmapPtr pFrame)
{
    WindowPtr pWin = sw->pWin;
    PixmapPtr pPixmap = sw->held.pPixmap;

    if (!pPixmap)
        return;

    if (pPixmap->drawable.width != pFrame->drawable.width ||
        pPixmap->drawable.height != pFrame->drawable.height)
        pPixmap = compServerCopyPixmap(pFrame);
    else if (!compServerCopyFrame(pFrame, pPixmap))
        pPixmap = NULL;

    if (!pPixmap)
        return;

    compServerDamageBox(pWin->drawable.pScreen, &sw->held.box);
    compServerSetHeldPixmap(sw, pPixmap, pWin->drawable.x, pWin->drawable.y);
    compServerDamageBox(pWin->drawable.pScreen, &sw->held.box);
}

void
compServerReleaseHeldFrame(CompServerWindowPtr sw)
{
    ScreenPtr pScreen = sw->pWin->drawable.pScreen;

    if (!sw->held.pPixmap)
        return;

    compServerDamageBox(pScreen, &sw->held.box);
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
    BoxPtr pHeld;

    if (!sw || !sw->held.pPixmap)
        return FALSE;

    pHeld = &sw->held.box;
    return pHeld->x1 < pBox->x2 && pBox->x1 < pHeld->x2 &&
        pHeld->y1 < pBox->y2 && pBox->y1 < pHeld->y2;
}

void
compServerDisplayedRegion(WindowPtr pWin, RegionPtr pRegion)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    if (sw && sw->held.pPixmap)
        RegionReset(pRegion, &sw->held.box);
    else
        RegionCopy(pRegion, &pWin->borderSize);
}

PixmapPtr
compServerDisplayedPixmap(WindowPtr pWin, int *x, int *y)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);
    PixmapPtr pPixmap;

    if (sw && sw->held.pPixmap) {
        *x = sw->held.box.x1;
        *y = sw->held.box.y1;
        return sw->held.pPixmap;
    }

    pPixmap = (*pWin->drawable.pScreen->GetWindowPixmap) (pWin);
    *x = pPixmap->screen_x;
    *y = pPixmap->screen_y;
    return pPixmap;
}
