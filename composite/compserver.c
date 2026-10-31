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

#include <stdio.h>
#include <string.h>
#include <X11/Xatom.h>

#include "compint.h"
#include "property.h"
#include "propertyst.h"
#include "selection.h"

#define COMP_SERVER_OPAQUE 0xffff

DevPrivateKeyRec CompServerWindowPrivateKeyRec;

Atom
compServerAtom(const char *name)
{
    return MakeAtom(name, strlen(name), TRUE);
}

Bool
compServerPropertyHasAtom(WindowPtr pWin, Atom property, Atom atom)
{
    PropertyPtr pProp;
    CARD32 *atoms;
    int i;

    if (dixLookupProperty(&pProp, pWin, property, serverClient,
                          DixReadAccess) != Success ||
        pProp->type != XA_ATOM || pProp->format != 32)
        return FALSE;

    atoms = pProp->data;
    for (i = 0; i < pProp->size; i++) {
        if (atoms[i] == atom)
            return TRUE;
    }
    return FALSE;
}

CompServerPtr
compServerGet(ScreenPtr pScreen)
{
    CompScreenPtr cs;

    if (!dixPrivateKeyRegistered(CompScreenPrivateKey))
        return NULL;

    cs = GetCompScreen(pScreen);
    return cs ? cs->server : NULL;
}

Bool
CompositeServerActive(ScreenPtr pScreen)
{
    return compServerGet(pScreen) != NULL;
}

Bool
CompositeIsServerRoot(WindowPtr pWin)
{
    return !pWin->parent && CompositeServerActive(pWin->drawable.pScreen);
}

static Bool
compServerOnlyRedirect(WindowPtr pWin)
{
    CompWindowPtr cw = GetCompWindow(pWin);
    CompClientWindowPtr ccw;

    if (pWin->redirectDraw != RedirectDrawAutomatic)
        return FALSE;

    for (ccw = cw->clients; ccw; ccw = ccw->next) {
        if (CLIENT_ID(ccw->id) != serverClient->index)
            return FALSE;
    }
    return TRUE;
}

Bool
CompositeIsServerRedirected(WindowPtr pWin)
{
    if (!pWin->parent || !CompositeServerActive(pWin->drawable.pScreen) ||
        !compServerOnlyRedirect(pWin))
        return FALSE;

    for (pWin = pWin->parent; pWin->parent; pWin = pWin->parent) {
        if (pWin->redirectDraw != RedirectDrawNone &&
            !compServerOnlyRedirect(pWin))
            return FALSE;
    }
    return TRUE;
}

WindowPtr
compServerToplevel(WindowPtr pWin)
{
    while (pWin->parent->parent)
        pWin = pWin->parent;
    return pWin;
}

static Bool
compServerPaintsWindow(WindowPtr pWin)
{
    return pWin->viewable && pWin->redirectDraw == RedirectDrawAutomatic;
}

Bool
compServerWindowHasAlpha(WindowPtr pWin)
{
    PictFormatPtr pFormat = PictureWindowFormat(pWin);

    return pFormat && pFormat->type == PictTypeDirect &&
        pFormat->direct.alphaMask;
}

static CARD16
compServerWindowOpacity(CompServerPtr server, WindowPtr pWin)
{
    PropertyPtr pProp;

    if (dixLookupProperty(&pProp, pWin, server->opacityAtom, serverClient,
                          DixReadAccess) != Success ||
        pProp->type != XA_CARDINAL || pProp->format != 32 || pProp->size < 1)
        return COMP_SERVER_OPAQUE;

    return *(CARD32 *) pProp->data >> 16;
}

void
compServerAddDamage(CompServerPtr server, RegionPtr pRegion)
{
    RegionUnion(&server->damage, &server->damage, pRegion);
}

void
compServerCollectDamage(WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);
    DamagePtr pDamage = GetCompWindow(pWin)->damage;
    RegionPtr pRegion = DamageRegion(pDamage);

    RegionTranslate(pRegion, pWin->drawable.x, pWin->drawable.y);
    compServerAddDamage(server, pRegion);
    DamageEmpty(pDamage);
}

static CompServerWindowPtr
compServerTrackWindow(CompServerPtr server, WindowPtr pWin)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    if (sw)
        return sw;

    sw = calloc(1, sizeof(*sw));
    if (!sw)
        return NULL;

    sw->pWin = pWin;
    RegionNull(&sw->painted);
    compServerInitFrameWait(sw);
    xorg_list_add(&sw->entry, &server->windows);
    dixSetPrivate(&pWin->devPrivates, CompServerWindowPrivateKey, sw);
    return sw;
}

static void
compServerFreeWindow(CompServerWindowPtr sw)
{
    compServerFreeFrameWait(sw);
    compServerFreeHeldFrame(sw);
    RegionUninit(&sw->painted);
    xorg_list_del(&sw->entry);
    free(sw);
}

static void
compServerUntrackWindow(CompServerPtr server, CompServerWindowPtr sw)
{
    compServerAddDamage(server, &sw->painted);
    dixSetPrivate(&sw->pWin->devPrivates, CompServerWindowPrivateKey, NULL);
    compServerFreeWindow(sw);
}

static void
compServerUpdateWindows(CompServerPtr server, WindowPtr pRoot)
{
    CompServerWindowPtr sw, tmp;
    WindowPtr pWin, pAbove = NULL;
    RegionRec displayed;

    xorg_list_for_each_entry(sw, &server->windows, entry)
        sw->seen = FALSE;

    RegionNull(&displayed);
    for (pWin = pRoot->firstChild; pWin; pWin = pWin->nextSib) {
        if (!compServerPaintsWindow(pWin))
            continue;

        sw = compServerTrackWindow(server, pWin);
        if (!sw) {
            compServerAddDamage(server, &pWin->borderSize);
            pAbove = pWin;
            continue;
        }

        compServerDisplayedRegion(pWin, &displayed);
        if (sw->pAbove != pAbove || !RegionEqual(&sw->painted, &displayed)) {
            compServerAddDamage(server, &sw->painted);
            compServerAddDamage(server, &displayed);
            RegionCopy(&sw->painted, &displayed);
            sw->pAbove = pAbove;
        }

        sw->seen = TRUE;
        pAbove = pWin;
    }
    RegionUninit(&displayed);

    xorg_list_for_each_entry_safe(sw, tmp, &server->windows, entry) {
        if (!sw->seen)
            compServerUntrackWindow(server, sw);
    }
}

void
compServerUnredirectManual(WindowPtr pWin)
{
    CompWindowPtr cw = GetCompWindow(pWin);
    CompClientWindowPtr ccw;

    if (!cw)
        return;

    for (ccw = cw->clients; ccw; ccw = ccw->next) {
        if (ccw->update == CompositeRedirectManual) {
            FreeResource(ccw->id, X11_RESTYPE_NONE);
            return;
        }
    }
}

void
compServerDestroyWindow(WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    if (!server)
        return;

    if (sw)
        compServerUntrackWindow(server, sw);
    compServerReleaseHeldWindow(server, pWin);
    compServerDestroyDirectWindow(server, pWin);
}

static void
compServerClaimCmSelection(ScreenPtr pScreen, CompServerPtr server)
{
    XID attrs[] = { TRUE };
    XID wid = FakeClientID(0);
    char name[32];
    WindowPtr pWin;
    int error;

    server->cmSelectionClaimed = TRUE;

    pWin = CreateWindow(wid, pScreen->root, -1, -1, 1, 1, 0, InputOnly,
                        CWOverrideRedirect, attrs, 0, serverClient,
                        CopyFromParent, &error);
    if (!pWin || !AddResource(wid, X11_RESTYPE_WINDOW, pWin))
        return;

    snprintf(name, sizeof(name), "_NET_WM_CM_S%d", pScreen->myNum);
    dixSetSelectionOwner(serverClient, MakeAtom(name, strlen(name), TRUE),
                         wid, currentTime);
}

void
CompositeServerFlush(ScreenPtr pScreen, RegionPtr pDamage)
{
    CompServerPtr server = compServerGet(pScreen);
    WindowPtr pRoot = pScreen->root;

    if (!pRoot)
        return;

    if (!server->cmSelectionClaimed)
        compServerClaimCmSelection(pScreen, server);

    compServerUpdateDeferred(server);
    compPaintChildrenToWindow(pRoot);
    compServerUpdateWindows(server, pRoot);

    if (RegionNil(&server->damage))
        return;

    DamageDamageRegion(&pRoot->drawable, &server->damage);
    RegionUnion(pDamage, pDamage, &server->damage);
    RegionEmpty(&server->damage);
}

static CARD16
compServerChannel(Pixel pixel, unsigned long mask, int offset)
{
    unsigned long max = mask >> offset;

    return max ? ((pixel & mask) >> offset) * 0xffff / max : 0;
}

static void
compServerPixelToColor(WindowPtr pRoot, Pixel pixel, xRenderColor *pColor)
{
    VisualPtr pVisual = pRoot->drawable.pScreen->visuals;

    while (pVisual->vid != wVisual(pRoot))
        pVisual++;

    pColor->red = compServerChannel(pixel, pVisual->redMask,
                                    pVisual->offsetRed);
    pColor->green = compServerChannel(pixel, pVisual->greenMask,
                                      pVisual->offsetGreen);
    pColor->blue = compServerChannel(pixel, pVisual->blueMask,
                                     pVisual->offsetBlue);
    pColor->alpha = 0xffff;
}

static void
compServerPaintBackground(WindowPtr pRoot, PicturePtr pDst, int x, int y,
                          RegionPtr pRegion)
{
    BoxPtr pExtents = RegionExtents(pRegion);
    xRectangle rect = {
        .x = pExtents->x1 - x,
        .y = pExtents->y1 - y,
        .width = pExtents->x2 - pExtents->x1,
        .height = pExtents->y2 - pExtents->y1,
    };

    SetPictureClipRegion(pDst, -x, -y, pRegion);

    if (pRoot->backgroundState == BackgroundPixmap) {
        XID repeat = RepeatNormal;
        PicturePtr pSrc;
        int error;

        pSrc = CreatePicture(0, &pRoot->background.pixmap->drawable,
                             PictureWindowFormat(pRoot), CPRepeat, &repeat,
                             serverClient, &error);
        if (pSrc) {
            CompositePicture(PictOpSrc, pSrc, NULL, pDst,
                             pExtents->x1, pExtents->y1, 0, 0,
                             rect.x, rect.y, rect.width, rect.height);
            FreePicture(pSrc, 0);
        }
    }
    else {
        xRenderColor color = { .alpha = 0xffff };

        if (pRoot->backgroundState == BackgroundPixel)
            compServerPixelToColor(pRoot, pRoot->background.pixel, &color);
        CompositeRects(PictOpSrc, pDst, &color, 1, &rect);
    }
}

static void
compServerPaintLayer(CompServerPtr server, CompServerLayerPtr pLayer,
                     PicturePtr pDst, int x, int y, void *closure)
{
    CompServerSourcePtr src = &pLayer->src;
    WindowPtr pWin = src->pWin;
    PixmapPtr pPixmap = src->pPixmap;
    CARD8 op = compServerWindowHasAlpha(pWin) ? PictOpOver : PictOpSrc;
    PicturePtr pSrc, pMask = NULL;
    int error;

    if (src->hdr && server->PaintHdr) {
        (*pPixmap->drawable.pScreen->SourceValidate) (&pPixmap->drawable, 0, 0,
                                                      pPixmap->drawable.width,
                                                      pPixmap->drawable.height,
                                                      IncludeInferiors);
        if ((*server->PaintHdr) (pDst, x, y, pWin, pPixmap, src->x, src->y,
                                 &pLayer->region, &pLayer->exposed,
                                 pLayer->opacity, closure))
            return;
    }

    pSrc = CreatePicture(0, &pPixmap->drawable, PictureWindowFormat(pWin),
                         0, NULL, serverClient, &error);
    if (!pSrc)
        return;

    if (pLayer->opacity != COMP_SERVER_OPAQUE) {
        xRenderColor alpha = { .alpha = pLayer->opacity };

        pMask = CreateSolidPicture(0, &alpha, &error);
        op = PictOpOver;
    }

    SetPictureClipRegion(pDst, -x, -y, &pLayer->region);
    CompositePicture(op, pSrc, pMask, pDst, 0, 0, 0, 0,
                     src->x - x, src->y - y,
                     pPixmap->drawable.width, pPixmap->drawable.height);

    if (pMask)
        FreePicture(pMask, 0);
    FreePicture(pSrc, 0);
}

static void
compServerSublayerRegion(WindowPtr pWin, RegionPtr pRegion)
{
    WindowPtr pAncestor;

    RegionCopy(pRegion, &pWin->winSize);
    if (pWin->redirectDraw == RedirectDrawNone)
        RegionIntersect(pRegion, pRegion, &pWin->borderClip);

    for (pAncestor = pWin; pAncestor->parent->parent;
         pAncestor = pAncestor->parent) {
        if (pAncestor->redirectDraw != RedirectDrawNone)
            RegionIntersect(pRegion, pRegion,
                            &GetCompWindow(pAncestor)->borderClip);
    }
}

static void
compServerAddLayer(CompServerLayersPtr pLayers, const CompServerSourceRec *src,
                   RegionPtr pArea, RegionPtr pRegion, CARD16 opacity)
{
    CompServerLayerPtr pLayer = &pLayers->layers[pLayers->count];
    Bool opaque = opacity == COMP_SERVER_OPAQUE &&
        !compServerWindowHasAlpha(src->pWin);

    RegionNull(&pLayer->region);
    RegionIntersect(&pLayer->region, pArea, pRegion);
    RegionSubtract(&pLayer->region, &pLayer->region, &pLayers->covered);
    if (RegionNil(&pLayer->region)) {
        RegionUninit(&pLayer->region);
        return;
    }

    pLayer->src = *src;
    pLayer->opacity = opacity;
    RegionNull(&pLayer->exposed);
    if (src->hdr && opaque)
        RegionSubtract(&pLayer->exposed, &pLayer->region, &pLayers->above);

    if (opaque)
        RegionUnion(&pLayers->covered, &pLayers->covered, &pLayer->region);
    RegionUnion(&pLayers->above, &pLayers->above, &pLayer->region);
    pLayers->count++;
}

static void
compServerWindowSource(WindowPtr pWin, Bool hdr, CompServerSourcePtr src)
{
    PixmapPtr pPixmap = (*pWin->drawable.pScreen->GetWindowPixmap) (pWin);

    *src = (CompServerSourceRec) {
        .pWin = pWin,
        .pPixmap = pPixmap,
        .x = pPixmap->screen_x,
        .y = pPixmap->screen_y,
        .hdr = hdr,
    };
}

static void
compServerAddSublayers(CompServerLayersPtr pLayers, WindowPtr pWin,
                       RegionPtr pRegion, CARD16 opacity, RegionPtr pHoles)
{
    CompServerSourceRec src;
    RegionRec sublayerArea;
    int i;

    RegionNull(&sublayerArea);
    for (i = 0; i < pLayers->numSublayers; i++) {
        CompServerSublayerPtr pSublayer = &pLayers->sublayers[i];

        if (pSublayer->pToplevel != pWin)
            continue;

        compServerSublayerRegion(pSublayer->pWin, &sublayerArea);
        RegionSubtract(&sublayerArea, &sublayerArea, pHoles);
        RegionUnion(pHoles, pHoles, &sublayerArea);
        compServerWindowSource(pSublayer->pWin, pSublayer->hdr, &src);
        compServerAddLayer(pLayers, &src, &sublayerArea, pRegion, opacity);
    }
    RegionUninit(&sublayerArea);
}

static void
compServerAddWindowLayers(CompServerPtr server, CompServerLayersPtr pLayers,
                          WindowPtr pWin, RegionPtr pRegion)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);
    CARD16 opacity = compServerWindowOpacity(server, pWin);
    CompServerSourceRec src;
    RegionRec area, holes;
    int i;

    if (sw && sw->held.layers) {
        for (i = 0; i < sw->held.count; i++)
            compServerAddLayer(pLayers, &sw->held.layers[i].src,
                               &sw->held.layers[i].region, pRegion, opacity);
        return;
    }

    RegionNull(&holes);
    compServerAddSublayers(pLayers, pWin, pRegion, opacity, &holes);

    RegionNull(&area);
    RegionSubtract(&area, &pWin->borderSize, &holes);
    compServerWindowSource(pWin, FALSE, &src);
    compServerAddLayer(pLayers, &src, &area, pRegion, opacity);
    RegionUninit(&area);
    RegionUninit(&holes);
}

static int
compServerWindowLayerCount(WindowPtr pWin)
{
    CompServerWindowPtr sw = GetCompServerWindow(pWin);

    return sw && sw->held.layers ? sw->held.count : 1;
}

static int
compServerCompareSublayers(const void *a, const void *b)
{
    const CompServerSublayerRec *pA = a, *pB = b;

    return pB->level - pA->level;
}

static Bool
compServerCollectSublayers(CompServerPtr server, CompServerLayersPtr pLayers)
{
    CompServerDirectWindowPtr dw;
    int count = 0;

    xorg_list_for_each_entry(dw, &server->directWindows, entry)
        count++;

    pLayers->numSublayers = 0;
    pLayers->sublayers = calloc(count, sizeof(*pLayers->sublayers));
    if (count && !pLayers->sublayers)
        return FALSE;

    xorg_list_for_each_entry(dw, &server->directWindows, entry) {
        CompServerSublayerPtr pSublayer =
            &pLayers->sublayers[pLayers->numSublayers];

        if (!dw->pWin->viewable)
            continue;

        pSublayer->pWin = dw->pWin;
        pSublayer->pToplevel = compServerToplevel(dw->pWin);
        pSublayer->level = compServerTreeLevel(dw->pWin);
        pSublayer->hdr = dw->hdr;
        pLayers->numSublayers++;
    }

    if (pLayers->numSublayers > 1)
        qsort(pLayers->sublayers, pLayers->numSublayers,
              sizeof(*pLayers->sublayers), compServerCompareSublayers);
    return TRUE;
}

static Bool
compServerInitLayers(CompServerPtr server, CompServerLayersPtr pLayers,
                     int count)
{
    if (!compServerCollectSublayers(server, pLayers))
        return FALSE;

    count += pLayers->numSublayers;
    pLayers->layers = calloc(count, sizeof(*pLayers->layers));
    if (count && !pLayers->layers) {
        free(pLayers->sublayers);
        return FALSE;
    }

    pLayers->count = 0;
    RegionNull(&pLayers->covered);
    RegionNull(&pLayers->above);
    return TRUE;
}

static Bool
compServerBuildLayers(CompServerPtr server, WindowPtr pFirst,
                      RegionPtr pRegion, CompServerLayersPtr pLayers)
{
    WindowPtr pWin;
    int count = 0;

    for (pWin = pFirst; pWin; pWin = pWin->nextSib)
        count += compServerWindowLayerCount(pWin);

    if (!compServerInitLayers(server, pLayers, count))
        return FALSE;

    for (pWin = pFirst; pWin; pWin = pWin->nextSib) {
        if (compServerPaintsWindow(pWin))
            compServerAddWindowLayers(server, pLayers, pWin, pRegion);
    }
    return TRUE;
}

Bool
compServerBuildWindowLayers(CompServerPtr server, WindowPtr pWin,
                            CompServerLayersPtr pLayers)
{
    if (!compServerInitLayers(server, pLayers,
                              compServerWindowLayerCount(pWin)))
        return FALSE;

    compServerAddWindowLayers(server, pLayers, pWin, &pWin->borderSize);
    return TRUE;
}

void
compServerFreeLayers(CompServerLayersPtr pLayers)
{
    int i;

    for (i = 0; i < pLayers->count; i++) {
        RegionUninit(&pLayers->layers[i].region);
        RegionUninit(&pLayers->layers[i].exposed);
    }
    RegionUninit(&pLayers->covered);
    RegionUninit(&pLayers->above);
    free(pLayers->layers);
    free(pLayers->sublayers);
}

static void
compServerPaint(ScreenPtr pScreen, PicturePtr pDst, int x, int y,
                RegionPtr pRegion, WindowPtr pAbove, void *closure)
{
    CompServerPtr server = compServerGet(pScreen);
    WindowPtr pRoot = pScreen->root;
    CompServerLayersRec layers;
    RegionRec background;
    int i;

    if (!pRoot || RegionNil(pRegion) ||
        !compServerBuildLayers(server,
                               pAbove ? pAbove->nextSib : pRoot->firstChild,
                               pRegion, &layers))
        return;

    RegionNull(&background);
    RegionSubtract(&background, pRegion, &layers.covered);
    if (RegionNotEmpty(&background))
        compServerPaintBackground(pRoot, pDst, x, y, &background);
    RegionUninit(&background);

    for (i = layers.count - 1; i >= 0; i--)
        compServerPaintLayer(server, &layers.layers[i], pDst, x, y, closure);

    SetPictureClipRegion(pDst, 0, 0, NULL);
    compServerFreeLayers(&layers);
}

void
CompositeServerPaint(ScreenPtr pScreen, PicturePtr pDst, int x, int y,
                     RegionPtr pRegion, void *closure)
{
    compServerPaint(pScreen, pDst, x, y, pRegion, NULL, closure);
}

static void
compServerPixmapBox(PixmapPtr pPixmap, BoxPtr pBox)
{
    pBox->x1 = pPixmap->screen_x;
    pBox->y1 = pPixmap->screen_y;
    pBox->x2 = pPixmap->screen_x + pPixmap->drawable.width;
    pBox->y2 = pPixmap->screen_y + pPixmap->drawable.height;
}

static void
compServerCopyWindowPixmap(WindowPtr pWin, PicturePtr pDst, int x, int y,
                           RegionPtr pCopied)
{
    PixmapPtr pPixmap = (*pWin->drawable.pScreen->GetWindowPixmap) (pWin);
    PicturePtr pSrc;
    BoxRec box;
    int error;

    pSrc = CreatePicture(0, &pPixmap->drawable, PictureWindowFormat(pWin),
                         0, NULL, serverClient, &error);
    if (!pSrc)
        return;

    CompositePicture(PictOpSrc, pSrc, NULL, pDst, 0, 0, 0, 0,
                     pPixmap->screen_x - x, pPixmap->screen_y - y,
                     pPixmap->drawable.width, pPixmap->drawable.height);
    FreePicture(pSrc, 0);

    compServerPixmapBox(pPixmap, &box);
    RegionReset(pCopied, &box);
}

void
compServerInitPixmap(WindowPtr pWin, PixmapPtr pPixmap)
{
    RegionRec region, copied;
    PicturePtr pDst;
    BoxRec box;
    int error;

    pDst = CreatePicture(0, &pPixmap->drawable, PictureWindowFormat(pWin),
                         0, NULL, serverClient, &error);
    if (!pDst)
        return;

    compServerPixmapBox(pPixmap, &box);
    RegionInit(&region, &box, 1);
    RegionNull(&copied);

    if (pWin->redirectDraw != RedirectDrawNone) {
        compServerCopyWindowPixmap(pWin, pDst, box.x1, box.y1, &copied);
        RegionSubtract(&region, &region, &copied);
    }

    if (RegionNotEmpty(&region))
        compServerPaint(pWin->drawable.pScreen, pDst, box.x1, box.y1, &region,
                        pWin, NULL);

    RegionUninit(&copied);
    RegionUninit(&region);
    FreePicture(pDst, 0);
}

static Bool
compServerHasHdrWindow(CompServerPtr server)
{
    CompServerDirectWindowPtr dw;

    xorg_list_for_each_entry(dw, &server->directWindows, entry) {
        if (dw->hdr)
            return TRUE;
    }
    return FALSE;
}

WindowPtr
CompositeServerHdrWindow(ScreenPtr pScreen, BoxPtr pBox)
{
    CompServerPtr server = compServerGet(pScreen);
    CompServerLayersRec layers;
    WindowPtr pHdr = NULL;
    RegionRec region;
    int i;

    if (!pScreen->root || !compServerHasHdrWindow(server))
        return NULL;

    RegionInit(&region, pBox, 1);
    if (compServerBuildLayers(server, pScreen->root->firstChild, &region,
                              &layers)) {
        for (i = 0; i < layers.count && !pHdr; i++) {
            if (layers.layers[i].src.hdr)
                pHdr = layers.layers[i].src.pWin;
        }
        compServerFreeLayers(&layers);
    }
    RegionUninit(&region);
    return pHdr;
}

static Bool
compServerWindowMatchesBox(WindowPtr pWin, BoxPtr pBox)
{
    return !wBorderWidth(pWin) &&
        pWin->drawable.x == pBox->x1 && pWin->drawable.y == pBox->y1 &&
        pWin->drawable.width == pBox->x2 - pBox->x1 &&
        pWin->drawable.height == pBox->y2 - pBox->y1 &&
        RegionContainsRect(&pWin->winSize, pBox) == rgnIN;
}

static WindowPtr
compServerTopmostChild(WindowPtr pParent, BoxPtr pBox)
{
    WindowPtr pChild;

    for (pChild = pParent->firstChild; pChild; pChild = pChild->nextSib) {
        if (pChild->viewable && pChild->drawable.class == InputOutput &&
            RegionContainsRect(&pChild->borderSize, pBox) != rgnOUT)
            return pChild;
    }
    return NULL;
}

static WindowPtr
compServerScanoutDescendant(WindowPtr pWin, BoxPtr pBox)
{
    WindowPtr pScanout = compServerWindowMatchesBox(pWin, pBox) ? pWin : NULL;
    WindowPtr pChild;

    while ((pChild = compServerTopmostChild(pWin, pBox)) &&
           pChild->redirectDraw != RedirectDrawManual &&
           RegionContainsRect(&pChild->borderSize, pBox) == rgnIN) {
        if (pChild->redirectDraw == RedirectDrawAutomatic &&
            compServerWindowMatchesBox(pChild, pBox))
            pScanout = pChild;
        pWin = pChild;
    }
    return pScanout;
}

static Bool
compServerSublayersMatchScanout(CompServerPtr server, WindowPtr pToplevel,
                                WindowPtr pScanout, BoxPtr pBox)
{
    ScreenPtr pScreen = pScanout->drawable.pScreen;
    PixmapPtr pPixmap = (*pScreen->GetWindowPixmap) (pScanout);
    CompServerDirectWindowPtr dw;

    xorg_list_for_each_entry(dw, &server->directWindows, entry) {
        RegionRec region;
        int overlap;

        if (dw->pWin == pScanout || !dw->pWin->viewable ||
            IsParent(dw->pWin, pScanout) ||
            compServerToplevel(dw->pWin) != pToplevel)
            continue;

        RegionNull(&region);
        compServerSublayerRegion(dw->pWin, &region);
        overlap = RegionContainsRect(&region, pBox);
        RegionUninit(&region);

        if (overlap != rgnOUT &&
            (overlap != rgnIN ||
             (*pScreen->GetWindowPixmap) (dw->pWin) != pPixmap))
            return FALSE;
    }
    return TRUE;
}

static WindowPtr
compServerScanoutWindow(ScreenPtr pScreen, BoxPtr pBox, WindowPtr pResizing)
{
    CompServerPtr server = compServerGet(pScreen);
    WindowPtr pWin, pScanout;

    for (pWin = pScreen->root->firstChild; pWin; pWin = pWin->nextSib) {
        Bool held = pWin != pResizing && compServerWindowHeld(pWin);
        int overlap;

        if (!compServerPaintsWindow(pWin))
            continue;

        overlap = RegionContainsRect(&pWin->borderSize, pBox);
        if (overlap == rgnOUT &&
            (!held || !compServerHeldFrameOverlaps(pWin, pBox)))
            continue;

        if (overlap != rgnIN || held ||
            compServerWindowOpacity(server, pWin) != COMP_SERVER_OPAQUE)
            return NULL;

        pScanout = compServerScanoutDescendant(pWin, pBox);
        if (!pScanout || compServerWindowHasAlpha(pScanout) ||
            !compServerSublayersMatchScanout(server, pWin, pScanout, pBox))
            return NULL;
        return pScanout;
    }

    return NULL;
}

WindowPtr
CompositeServerScanoutWindow(ScreenPtr pScreen, BoxPtr pBox)
{
    return compServerScanoutWindow(pScreen, pBox, NULL);
}

Bool
CompositeServerCanScanoutWindow(WindowPtr pWin, BoxPtr pBox)
{
    return pWin->parent &&
        compServerScanoutWindow(pWin->drawable.pScreen, pBox,
                                compServerToplevel(pWin)) == pWin;
}

static PixmapPtr
compServerComposeImage(ScreenPtr pScreen, int x, int y, int w, int h)
{
    WindowPtr pRoot = pScreen->root;
    BoxRec box = { .x1 = x, .y1 = y, .x2 = x + w, .y2 = y + h };
    RegionRec region;
    PixmapPtr pPixmap;
    PicturePtr pDst;
    int error;

    pPixmap = (*pScreen->CreatePixmap) (pScreen, w, h, pRoot->drawable.depth,
                                        CREATE_PIXMAP_USAGE_SCRATCH);
    if (!pPixmap)
        return NULL;

    pDst = CreatePicture(0, &pPixmap->drawable, PictureWindowFormat(pRoot),
                         0, NULL, serverClient, &error);
    if (!pDst) {
        dixDestroyPixmap(pPixmap, 0);
        return NULL;
    }

    RegionInit(&region, &box, 1);
    compServerPaint(pScreen, pDst, x, y, &region, NULL, NULL);
    RegionUninit(&region);
    FreePicture(pDst, 0);
    return pPixmap;
}

static void
compServerGetImage(DrawablePtr pDrawable, int sx, int sy, int w, int h,
                   unsigned int format, unsigned long planeMask,
                   char *pdstLine)
{
    ScreenPtr pScreen = pDrawable->pScreen;
    CompServerPtr server = compServerGet(pScreen);
    PixmapPtr pPixmap = NULL;

    if (pDrawable->type == DRAWABLE_WINDOW &&
        !((WindowPtr) pDrawable)->parent)
        pPixmap = compServerComposeImage(pScreen, pDrawable->x + sx,
                                         pDrawable->y + sy, w, h);

    pScreen->GetImage = server->GetImage;
    if (pPixmap)
        (*pScreen->GetImage) (&pPixmap->drawable, 0, 0, w, h, format,
                              planeMask, pdstLine);
    else
        (*pScreen->GetImage) (pDrawable, sx, sy, w, h, format, planeMask,
                              pdstLine);
    server->GetImage = pScreen->GetImage;
    pScreen->GetImage = compServerGetImage;

    if (pPixmap)
        dixDestroyPixmap(pPixmap, 0);
}

static void
compServerClearToBackground(WindowPtr pWin, int x, int y, int w, int h,
                            Bool generateExposures)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    CompServerPtr server = compServerGet(pScreen);

    if (!pWin->parent)
        compServerAddDamage(server, &pWin->winSize);

    pScreen->ClearToBackground = server->ClearToBackground;
    (*pScreen->ClearToBackground) (pWin, x, y, w, h, generateExposures);
    server->ClearToBackground = pScreen->ClearToBackground;
    pScreen->ClearToBackground = compServerClearToBackground;
}

static void
compServerPropertyState(CallbackListPtr *pcbl, void *closure, void *data)
{
    ScreenPtr pScreen = closure;
    PropertyStateRec *rec = data;
    WindowPtr pWin = rec->win;
    CompServerPtr server = compServerGet(pScreen);

    if (pWin->drawable.pScreen != pScreen)
        return;

    if (rec->prop->propertyName == server->opacityAtom) {
        if (pWin->parent == pScreen->root && pWin->viewable)
            compServerAddDamage(server, &pWin->borderSize);
    }
    else
        compServerFramePropertyChanged(server, rec);
}

Bool
CompositeServerInit(ScreenPtr pScreen, CompositeServerPaintHdrProcPtr PaintHdr)
{
    static const char opacity_name[] = "_NET_WM_WINDOW_OPACITY";
    CompScreenPtr cs;
    CompServerPtr server;

    if (!dixPrivateKeyRegistered(CompScreenPrivateKey))
        return FALSE;

    cs = GetCompScreen(pScreen);
    if (!cs)
        return FALSE;

    if (!compServerInitFrameSync())
        return FALSE;

    server = calloc(1, sizeof(*server));
    if (!server)
        return FALSE;

    if (!AddCallback(&PropertyStateCallback, compServerPropertyState,
                     pScreen)) {
        free(server);
        return FALSE;
    }

    RegionNull(&server->damage);
    xorg_list_init(&server->windows);
    xorg_list_init(&server->directWindows);
    compServerInitFrameTiming(server);
    server->PaintHdr = PaintHdr;
    server->opacityAtom = MakeAtom(opacity_name, strlen(opacity_name), TRUE);

    server->GetImage = pScreen->GetImage;
    pScreen->GetImage = compServerGetImage;

    server->ClearToBackground = pScreen->ClearToBackground;
    pScreen->ClearToBackground = compServerClearToBackground;

    cs->server = server;
    return TRUE;
}

void
compServerCloseScreen(ScreenPtr pScreen)
{
    CompScreenPtr cs = GetCompScreen(pScreen);
    CompServerPtr server = cs->server;
    CompServerWindowPtr sw, tmp;

    if (!server)
        return;

    DeleteCallback(&PropertyStateCallback, compServerPropertyState, pScreen);
    compServerCloseFrameSync();
    compServerCloseFrameTiming(server);

    pScreen->GetImage = server->GetImage;
    pScreen->ClearToBackground = server->ClearToBackground;

    xorg_list_for_each_entry_safe(sw, tmp, &server->windows, entry)
        compServerFreeWindow(sw);

    compServerFreeDirectWindows(server);

    RegionUninit(&server->damage);
    free(server);
    cs->server = NULL;
}
